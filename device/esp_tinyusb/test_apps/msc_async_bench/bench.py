#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Measure CDC echo latency while the MSC drive of the same device is busy.

Run once per firmware build (async IO off / on) and compare the tables.
See README.md for the device-side protocol and setup.
"""

import argparse
import csv
import hashlib
import os
import statistics
import struct
import sys
import threading
import time
from dataclasses import dataclass, field

import serial

PROBE_LEN = 8
PROBE_MARK = 0xAA  # anything but the 0xFF control prefix
CTRL = 0xFF
IO_CHUNK = 64 * 1024


@dataclass
class Latency:
    samples_ms: list = field(default_factory=list)
    timeouts: int = 0

    def summary(self) -> dict:
        s = sorted(self.samples_ms)
        if not s:
            return {'n': 0, 'p50': float('nan'), 'p99': float('nan'), 'max': float('nan'), 'timeouts': self.timeouts}
        p99 = s[min(len(s) - 1, int(len(s) * 0.99))]
        return {'n': len(s), 'p50': statistics.median(s), 'p99': p99, 'max': s[-1], 'timeouts': self.timeouts}


@dataclass
class Load:
    write_mbps: list = field(default_factory=list)
    read_mbps: list = field(default_factory=list)
    verify_fail: int = 0
    rounds: int = 0
    error: str = ''


def ctrl(set: serial.Serial, payload: bytes) -> str:
    set.reset_input_buffer()
    set.write(bytes([CTRL]) + payload)
    line = set.readline().decode(errors='replace').strip()
    if not line:
        raise RuntimeError(f'No reply to control command {payload!r}; is this the bench firmware CDC port?')
    return line


def set_delay(set: serial.Serial, delay_ms: int) -> None:
    reply = ctrl(set, b'D' + struct.pack('<H', delay_ms))
    if reply != f'D {delay_ms}':
        raise RuntimeError(f'Unexpected reply to set-delay: {reply!r}')


def get_stats(set: serial.Serial) -> tuple:
    _, reads, writes, delay = ctrl(set, b'S').split()
    return int(reads), int(writes), int(delay)


def get_info(set: serial.Serial) -> dict:
    """Storage type, MSC IO mode and SD bus clock of the flashed firmware."""
    parts = ctrl(set, b'I').split()
    if len(parts) != 4 or parts[0] != 'I':
        # Firmware built before the 'I' request existed
        return {'storage': 'unknown', 'io_mode': 'unknown', 'sd_khz': 0}
    return {'storage': parts[1], 'io_mode': parts[2], 'sd_khz': int(parts[3])}


def get_worker_stack(set: serial.Serial) -> tuple[int, int]:
    """Peak stack use of the async IO worker since boot and its stack size, in bytes. (-1, -1): no worker."""
    parts = ctrl(set, b'W').split()
    if len(parts) != 3 or parts[0] != 'W':
        return -1, -1
    return int(parts[1]), int(parts[2])


def fmt_stack(used: int, size: int) -> str:
    return f'{used}/{size} ({100 * used / size:.0f}%)' if used >= 0 and size > 0 else '-'



def probe(set: serial.Serial, stop: threading.Event, interval_s: float, timeout_s: float) -> Latency:
    lat = Latency()
    seq = 0
    set.timeout = timeout_s
    set.reset_input_buffer()
    while not stop.is_set():
        seq = (seq + 1) & 0xFFFFFFFF
        pkt = bytes([PROBE_MARK]) + struct.pack('<I', seq) + b'\x55\x55\x55'
        t0 = time.perf_counter()
        set.write(pkt)
        rx = set.read(PROBE_LEN)
        dt_ms = (time.perf_counter() - t0) * 1000.0
        if rx == pkt:
            lat.samples_ms.append(dt_ms)
        else:
            lat.timeouts += 1
            time.sleep(0.05)
            set.reset_input_buffer()
        rest = interval_s - (time.perf_counter() - t0)
        if rest > 0:
            time.sleep(rest)
    return lat


def _no_cache(fd: int) -> None:
    import fcntl
    if hasattr(fcntl, 'F_NOCACHE'):  # macOS
        fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)


def _drop_cache(fd: int) -> None:
    if hasattr(os, 'posix_fadvise'):  # Linux
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)


def _full_sync(fd: int) -> None:
    import fcntl
    if hasattr(fcntl, 'F_FULLFSYNC'):
        fcntl.fcntl(fd, fcntl.F_FULLFSYNC)
    else:
        os.fsync(fd)


def msc_load(path: str, size: int, stop: threading.Event, load: Load) -> None:
    try:
        while not stop.is_set():
            data = os.urandom(size)
            digest = hashlib.sha256(data).digest()

            t0 = time.perf_counter()
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
            try:
                _no_cache(fd)
                for off in range(0, size, IO_CHUNK):
                    os.write(fd, data[off:off + IO_CHUNK])
                _full_sync(fd)
                _drop_cache(fd)
            finally:
                os.close(fd)
            load.write_mbps.append(size / (time.perf_counter() - t0) / 1e6)

            t0 = time.perf_counter()
            fd = os.open(path, os.O_RDONLY)
            try:
                _no_cache(fd)
                chunks = []
                while True:
                    b = os.read(fd, IO_CHUNK)
                    if not b:
                        break
                    chunks.append(b)
            finally:
                os.close(fd)
            load.read_mbps.append(size / (time.perf_counter() - t0) / 1e6)

            if hashlib.sha256(b''.join(chunks)).digest() != digest:
                load.verify_fail += 1
            load.rounds += 1
    except OSError as e:
        load.error = str(e)


def run_phase(set, duration_s, interval_s, timeout_s, load_path=None, load_size=0):
    stop = threading.Event()
    load = Load()
    loader = None
    if load_path:
        loader = threading.Thread(target=msc_load, args=(load_path, load_size, stop, load), daemon=True)
        loader.start()
    timer = threading.Timer(duration_s, stop.set)
    timer.start()
    lat = probe(set, stop, interval_s, timeout_s)
    if loader:
        loader.join()  # let the current write/read round finish
    return lat, load


def fmt(v: float) -> str:
    return f'{v:8.2f}'


def _write_file(path: str, data: bytes) -> str:
    """Write and fully sync a file. Return the stage that raised an error, or '' if none did."""
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    except OSError:
        return 'open'
    try:
        _no_cache(fd)
        try:
            for off in range(0, len(data), IO_CHUNK):
                os.write(fd, data[off:off + IO_CHUNK])
        except OSError:
            return 'write'
        try:
            _full_sync(fd)
        except OSError:
            return 'fsync'
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
    return ''


def inject_errors(set: serial.Serial, volume: str, file_kb: int) -> None:
    """Make one device-side write fail and report where the host notices it."""
    set_delay(set, 0)
    reply = ctrl(set, b'F' + struct.pack('<H', 1))
    if reply != 'F 1':
        raise RuntimeError(f'Unexpected reply to fail-writes: {reply!r}')

    path = os.path.join(volume, 'BENCH_E.BIN')
    first = _write_file(path, os.urandom(file_kb * 1024))
    follow_up = _write_file(path, os.urandom(64 * 1024))
    ctrl(set, b'F' + struct.pack('<H', 0))

    print(f'Injected 1 failing device write while writing {file_kb} KiB:')
    print(f'  same file : {"error at " + first if first else "no error"}')
    print(f'  next write: {"error at " + follow_up if follow_up else "no error"}')
    if not first and not follow_up:
        print('  -> the failure was NOT reported to the host')
    stack_used, stack_size = get_worker_stack(set)
    if stack_used >= 0:
        print(f'Async IO worker: peak stack use since boot {fmt_stack(stack_used, stack_size)} bytes')
    try:
        os.remove(path)
    except OSError:
        pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True, help='CDC port of the bench device, e.g. /dev/cu.usbmodem1101')
    ap.add_argument('--volume', required=True, help='Mount point of the MSC drive, e.g. /Volumes/NO\\ NAME')
    ap.add_argument('--label', default='', help='Free text stored in the CSV, e.g. "async" or "sync"')
    ap.add_argument('--delays', default='0,2,5,10,20', help='Comma separated storage delays in ms (default: %(default)s)')
    ap.add_argument('--duration', type=float, default=8.0, help='Seconds per phase (default: %(default)s)')
    ap.add_argument('--file-kb', type=int, default=1024, help='Size of the file written/read under load (default: %(default)s)')
    ap.add_argument('--probe-hz', type=float, default=200.0, help='Echo probes per second (default: %(default)s)')
    ap.add_argument('--timeout', type=float, default=2.0, help='Echo timeout in seconds (default: %(default)s)')
    ap.add_argument('--csv', help='Append results to this CSV file')
    ap.add_argument('--inject-errors', action='store_true',
                    help='Skip the latency sweep; make one device write fail and report whether the host sees it')
    args = ap.parse_args()

    if not os.path.ismount(args.volume):
        print(f'{args.volume} is not a mounted volume', file=sys.stderr)
        return 1

    set = serial.Serial(args.port, 115200, timeout=1.0)
    time.sleep(0.2)

    if args.inject_errors:
        inject_errors(set, args.volume, args.file_kb)
        set.close()
        return 0

    load_path = os.path.join(args.volume, 'BENCH.BIN')
    delays = [int(d) for d in args.delays.split(',')]
    interval = 1.0 / args.probe_hz
    info = get_info(set)

    rows = []
    hdr = (f'{"delay":>5} {"phase":>5} | {"p50 ms":>8} {"p99 ms":>8} {"max ms":>8} {"n":>6} {"tmo":>4} |'
           f' {"wr MB/s":>8} {"rd MB/s":>8} {"rounds":>6} {"verify":>6} {"MSC rd/wr ops":>14} {"stack used B":>16}')
    sd_clock = f', SD {info["sd_khz"]} kHz' if info['storage'] == 'sd' else ''
    print(f'label={args.label!r} storage={info["storage"]}{sd_clock} io_mode={info["io_mode"]}'
          f' file={args.file_kb} KiB, {args.duration:.0f} s per phase')
    if info['storage'] == 'sd' and any(delays):
        print('note: on the SD card, delay > 0 is added on top of the card\'s own access time')
    print(hdr)
    print('-' * len(hdr))
    for delay in delays:
        set_delay(set, delay)
        for phase in ('idle', 'load'):
            ctrl(set, b'R')
            lat, load = run_phase(set, args.duration, interval, args.timeout,
                                  load_path if phase == 'load' else None, args.file_kb * 1024)
            reads, writes, _ = get_stats(set)
            stack_used, stack_size = get_worker_stack(set)
            s = lat.summary()
            wr = statistics.mean(load.write_mbps) if load.write_mbps else float('nan')
            rd = statistics.mean(load.read_mbps) if load.read_mbps else float('nan')
            verify = '-' if phase == 'idle' else ('OK' if load.verify_fail == 0 and load.rounds else 'FAIL')
            print(f'{delay:>5} {phase:>5} | {fmt(s["p50"])} {fmt(s["p99"])} {fmt(s["max"])} {s["n"]:>6} {s["timeouts"]:>4} |'
                  f' {fmt(wr)} {fmt(rd)} {load.rounds:>6} {verify:>6} {f"{reads}/{writes}":>14} {fmt_stack(stack_used, stack_size):>16}')
            if load.error:
                print(f'      load error: {load.error}')
            rows.append({'label': args.label, **info, 'delay_ms': delay, 'phase': phase, **s,
                         'write_mbps': wr, 'read_mbps': rd, 'rounds': load.rounds,
                         'verify_fail': load.verify_fail, 'msc_reads': reads, 'msc_writes': writes,
                         'worker_stack_used': stack_used, 'worker_stack_size': stack_size})
    set_delay(set, 0)
    set.close()
    if os.path.exists(load_path):
        os.remove(load_path)

    if args.csv:
        append_csv(args.csv, rows)
        print(f'Appended {len(rows)} rows to {args.csv}')
    return 0


def append_csv(path: str, rows: list) -> None:
    """Append rows. If the file has different columns (older bench.py), rewrite it with the union of both."""
    fields = list(rows[0].keys())
    old_rows = []
    if os.path.exists(path):
        with open(path, newline='') as f:
            reader = csv.DictReader(f)
            old_fields = reader.fieldnames or []
            if old_fields == fields:
                old_rows = None
            else:
                old_rows = list(reader)
                fields = old_fields + [k for k in fields if k not in old_fields]
    if old_rows is None:
        with open(path, 'a', newline='') as f:
            csv.DictWriter(f, fieldnames=fields).writerows(rows)
        return
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(old_rows)
        w.writerows(rows)


if __name__ == '__main__':
    sys.exit(main())
