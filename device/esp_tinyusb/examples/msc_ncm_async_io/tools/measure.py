#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Measure USB NCM latency and throughput of the msc_ncm_async_io example while copying to its USB drive.

Phases (each copy writes a new file of --size-mb to the drive):
  copy           copy only, no network traffic         -> MSC write rate alone
  latency_idle   UDP echo for --idle-s seconds         -> baseline latency
  latency_copy   UDP echo during a copy                -> latency while MSC is busy
  tcp_*_idle     TCP for --idle-s seconds              -> baseline throughput
  tcp_*_copy     TCP during a copy                     -> throughput while MSC is busy

Standard library only. Run once per firmware build (async IO OFF and ON) and compare the rows in --csv.
"""

import argparse
import csv
import fcntl
import json
import os
import socket
import statistics
import struct
import threading
import time
import urllib.request

UDP_ECHO_PORT = 7
TCP_SINK_PORT = 5001
TCP_SOURCE_PORT = 5002
CHUNK = 1024 * 1024
TEST_FILE = 'msc_ncm_test.bin'
# Device counters reported per phase as the difference between the start and end of the phase
TX_COUNTERS = ('ncm_tx_sent', 'ncm_tx_busy_drops', 'ncm_tx_timeout_drops', 'ncm_tx_busy_retries')


def get_status(ip):
    with urllib.request.urlopen(f'http://{ip}/status', timeout=3) as resp:
        return json.load(resp)


def copy_to_drive(volume, size_mb):
    """Writes size_mb to the drive, bypassing the host page cache. Returns MB/s."""
    path = os.path.join(volume, TEST_FILE)
    data = bytes(CHUNK)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        if hasattr(fcntl, 'F_NOCACHE'):
            fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)
        start = time.monotonic()
        for _ in range(size_mb):
            os.write(fd, data)
        if hasattr(fcntl, 'F_FULLFSYNC'):
            fcntl.fcntl(fd, fcntl.F_FULLFSYNC)
        else:
            os.fsync(fd)
        elapsed = time.monotonic() - start
    finally:
        os.close(fd)
        os.remove(path)
    return size_mb * CHUNK / 1e6 / elapsed


class LatencyProbe(threading.Thread):
    """Sends a UDP echo request every interval and records the round-trip times."""

    def __init__(self, ip, interval_s):
        super().__init__(daemon=True)
        self.addr = (ip, UDP_ECHO_PORT)
        self.interval_s = interval_s
        self.stop_event = threading.Event()
        self.sent = 0
        self.rtts_ms = []
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.2)
        self.receiving = True

    def run(self):
        receiver = threading.Thread(target=self._receive, daemon=True)
        receiver.start()
        next_t = time.monotonic()
        while not self.stop_event.is_set():
            self.sock.sendto(struct.pack('!Id', self.sent, time.monotonic()), self.addr)
            self.sent += 1
            next_t += self.interval_s
            time.sleep(max(0.0, next_t - time.monotonic()))
        time.sleep(1.0)  # late replies still count
        self.receiving = False
        receiver.join()

    def _receive(self):
        while self.receiving:
            try:
                data, _ = self.sock.recvfrom(64)
            except socket.timeout:
                continue
            _, t_send = struct.unpack('!Id', data[:12])
            self.rtts_ms.append((time.monotonic() - t_send) * 1000)

    def result(self, stall_ms):
        r = sorted(self.rtts_ms)
        if not r:
            return {'sent': self.sent, 'lost': self.sent}
        return {
            'sent': self.sent,
            'lost': self.sent - len(r),
            'rtt_min_ms': round(r[0], 2),
            'rtt_avg_ms': round(statistics.fmean(r), 2),
            'rtt_p50_ms': round(r[len(r) // 2], 2),
            'rtt_p99_ms': round(r[min(len(r) - 1, int(len(r) * 0.99))], 2),
            'rtt_max_ms': round(r[-1], 2),
            f'rtt_over_{stall_ms}ms': sum(1 for x in r if x > stall_ms),
        }


class TcpProbe(threading.Thread):
    """Streams TCP to (up) or from (down) the device and samples throughput every second."""

    def __init__(self, ip, direction):
        super().__init__(daemon=True)
        self.ip = ip
        self.direction = direction
        self.stop_event = threading.Event()
        self.total = 0
        self.samples_mbps = []

    def run(self):
        port = TCP_SINK_PORT if self.direction == 'up' else TCP_SOURCE_PORT
        sock = socket.create_connection((self.ip, port), timeout=5)
        sock.settimeout(1.0)
        buf = bytes(64 * 1024)
        start = last_t = time.monotonic()
        last_total = 0
        while not self.stop_event.is_set():
            try:
                if self.direction == 'up':
                    self.total += sock.send(buf)
                else:
                    data = sock.recv(64 * 1024)
                    if not data:
                        break
                    self.total += len(data)
            except socket.timeout:
                pass
            now = time.monotonic()
            if now - last_t >= 1.0:
                self.samples_mbps.append((self.total - last_total) * 8 / 1e6 / (now - last_t))
                last_t, last_total = now, self.total
        self.elapsed = time.monotonic() - start
        sock.close()

    def result(self):
        res = {f'tcp_{self.direction}_avg_mbps': round(self.total * 8 / 1e6 / self.elapsed, 1)}
        if self.samples_mbps:
            res[f'tcp_{self.direction}_min_1s_mbps'] = round(min(self.samples_mbps), 1)
        return res


def run_probe(probe, volume, size_mb, idle_s):
    """Runs the probe during a copy (volume set) or for idle_s seconds. Returns copy MB/s or None."""
    probe.start()
    time.sleep(0.5)  # let the probe settle
    rate = None
    if volume:
        rate = copy_to_drive(volume, size_mb)
    else:
        time.sleep(idle_s)
    probe.stop_event.set()
    probe.join()
    return rate


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--volume', required=True, help='mount point of the device USB drive, e.g. /Volumes/NO\\ NAME')
    ap.add_argument('--ip', default='192.168.4.1')
    ap.add_argument('--size-mb', type=int, default=100, help='MB written per copy (default 100)')
    ap.add_argument('--idle-s', type=float, default=5, help='duration of the idle phases (default 5)')
    ap.add_argument('--interval-ms', type=float, default=10, help='UDP echo interval (default 10)')
    ap.add_argument('--stall-ms', type=int, default=10, help='RTT counted as a stall (default 10)')
    ap.add_argument('--tcp', choices=['up', 'down', 'both', 'none'], default='both')
    ap.add_argument('--csv', default='results.csv', help='append results to this file (default results.csv)')
    args = ap.parse_args()

    status = get_status(args.ip)
    mode = f"async {status['async_io']}"
    if status.get('ncm_tx_retry_busy'):
        mode += ' +tx_retry'
    print(f"Device: {status['storage']}, {mode}, medium delay {status['medium_delay_ms']} ms\n")

    phases = [('copy', None)]
    phases += [('latency_idle', 'lat'), ('latency_copy', 'lat')]
    dirs = {'both': ['up', 'down'], 'none': []}.get(args.tcp, [args.tcp])
    for d in dirs:
        phases += [(f'tcp_{d}_idle', d), (f'tcp_{d}_copy', d)]

    rows = []
    for name, kind in phases:
        print(f'--- {name} ...', flush=True)
        before = get_status(args.ip)
        during_copy = name == 'copy' or name.endswith('_copy')
        volume = args.volume if during_copy else None
        if kind is None:
            res = {'copy_mbps': round(copy_to_drive(args.volume, args.size_mb), 1)}
        else:
            probe = LatencyProbe(args.ip, args.interval_ms / 1000) if kind == 'lat' else TcpProbe(args.ip, kind)
            rate = run_probe(probe, volume, args.size_mb, args.idle_s)
            res = probe.result(args.stall_ms) if kind == 'lat' else probe.result()
            if rate is not None:
                res['copy_mbps'] = round(rate, 1)
        after = get_status(args.ip)
        for key in TX_COUNTERS:
            if key in after:
                res[key] = after[key] - before[key]
        if 'ncm_tx_max_send_us' in after:
            res['ncm_tx_max_send_us_total'] = after['ncm_tx_max_send_us']
        print('    ' + ', '.join(f'{k}={v}' for k, v in res.items()))
        rows.append({'mode': mode, 'storage': status['storage'], 'phase': name,
                     'result': json.dumps(res)})

    status = get_status(args.ip)
    print(f"\nNCM TX frames dropped by the device so far: {status['ncm_tx_drops']}")

    new_file = not os.path.exists(args.csv)
    with open(args.csv, 'a', newline='') as f:
        w = csv.DictWriter(f, fieldnames=['mode', 'storage', 'phase', 'result'])
        if new_file:
            w.writeheader()
        w.writerows(rows)
    print(f'Results appended to {args.csv}')


if __name__ == '__main__':
    main()
