#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Check CONFIG_TINYUSB_MSC_ASYNC_IO against the msc_async_io test app firmware.

Runs on macOS and Linux. Uses the CDC port of the device and the MSC drive mounted by the host OS.
See README.md for building, flashing and the device-side control protocol.

Checks, for each storage delay:
  - data written to the MSC drive reads back unchanged
  - CDC echo latency while the MSC drive is busy: stays low with async IO, grows to about
    the storage delay without it
With --inject-errors instead: one failed storage write is reported to the host as an I/O error.

Exits with 0 when all checks pass, 1 otherwise.
"""

import argparse
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
            return {'n': 0, 'p50': float('nan'), 'p90': float('nan'), 'max': float('nan'), 'timeouts': self.timeouts}
        p90 = s[min(len(s) - 1, int(len(s) * 0.9))]
        return {'n': len(s), 'p50': statistics.median(s), 'p90': p90, 'max': s[-1], 'timeouts': self.timeouts}


@dataclass
class Load:
    write_mbps: list = field(default_factory=list)
    read_mbps: list = field(default_factory=list)
    verify_fail: int = 0
    rounds: int = 0
    error: str = ''


def ctrl(cdc: serial.Serial, payload: bytes) -> str:
    cdc.reset_input_buffer()
    cdc.write(bytes([CTRL]) + payload)
    line = cdc.readline().decode(errors='replace').strip()
    if not line:
        raise RuntimeError(f'No reply to control command {payload!r}; is this the msc_async_io test app CDC port?')
    return line


def set_delay(cdc: serial.Serial, delay_ms: int) -> None:
    reply = ctrl(cdc, b'D' + struct.pack('<H', delay_ms))
    if reply != f'D {delay_ms}':
        raise RuntimeError(f'Unexpected reply to set-delay: {reply!r}')


def set_fail_writes(cdc: serial.Serial, count: int) -> None:
    reply = ctrl(cdc, b'F' + struct.pack('<H', count))
    if reply != f'F {count}':
        raise RuntimeError(f'Unexpected reply to fail-writes: {reply!r}')


def get_stats(cdc: serial.Serial) -> tuple:
    _, reads, writes, delay = ctrl(cdc, b'S').split()
    return int(reads), int(writes), int(delay)


def get_io_mode(cdc: serial.Serial) -> str:
    """'sync' or 'async', as built into the flashed firmware."""
    parts = ctrl(cdc, b'I').split()
    if len(parts) != 2 or parts[0] != 'I':
        raise RuntimeError(f'Unexpected reply to info: {" ".join(parts)!r}')
    return parts[1]


def get_worker_stack(cdc: serial.Serial) -> str:
    """Peak stack use and stack size of the async IO worker, or '-' without async IO."""
    parts = ctrl(cdc, b'W').split()
    if len(parts) != 3 or parts[0] != 'W' or parts[1] == '-1':
        return '-'
    return f'{parts[1]}/{parts[2]} B'


def probe(cdc: serial.Serial, stop: threading.Event, interval_s: float, timeout_s: float) -> Latency:
    lat = Latency()
    seq = 0
    cdc.timeout = timeout_s
    cdc.reset_input_buffer()
    while not stop.is_set():
        seq = (seq + 1) & 0xFFFFFFFF
        pkt = bytes([PROBE_MARK]) + struct.pack('<I', seq) + b'\x55\x55\x55'
        t0 = time.perf_counter()
        cdc.write(pkt)
        rx = cdc.read(PROBE_LEN)
        dt_ms = (time.perf_counter() - t0) * 1000.0
        if rx == pkt:
            lat.samples_ms.append(dt_ms)
        else:
            lat.timeouts += 1
            time.sleep(0.05)
            cdc.reset_input_buffer()
        rest = interval_s - (time.perf_counter() - t0)
        if rest > 0:
            time.sleep(rest)
    return lat


# Keep the host page cache out of the way, so every write and read reaches the device.
# macOS: F_NOCACHE and F_FULLFSYNC. Linux: fsync, then drop the cached pages before reading back.

def _no_cache(fd: int) -> None:
    import fcntl
    if hasattr(fcntl, 'F_NOCACHE'):
        fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)


def _drop_cache(fd: int) -> None:
    if hasattr(os, 'posix_fadvise'):
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)


def _full_sync(fd: int) -> None:
    import fcntl
    if hasattr(fcntl, 'F_FULLFSYNC'):
        fcntl.fcntl(fd, fcntl.F_FULLFSYNC)
    else:
        os.fsync(fd)


def _read_file(path: str) -> bytes:
    """Read a whole file from the device, bypassing the host page cache."""
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
    return b''.join(chunks)


def msc_load(path: str, size: int, stop: threading.Event, load: Load) -> None:
    """Write a random file, read it back and compare, until stopped."""
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
            readback = _read_file(path)
            load.read_mbps.append(size / (time.perf_counter() - t0) / 1e6)

            if hashlib.sha256(readback).digest() != digest:
                load.verify_fail += 1
            load.rounds += 1
    except OSError as e:
        load.error = str(e)


def run_phase(cdc, duration_s, interval_s, timeout_s, load_path=None, load_size=0):
    stop = threading.Event()
    load = Load()
    loader = None
    if load_path:
        loader = threading.Thread(target=msc_load, args=(load_path, load_size, stop, load), daemon=True)
        loader.start()
    timer = threading.Timer(duration_s, stop.set)
    timer.start()
    lat = probe(cdc, stop, interval_s, timeout_s)
    if loader:
        loader.join()  # let the current write/read round finish
    return lat, load


def fmt(v: float) -> str:
    return f'{v:8.2f}'


def latency_ok(io_mode: str, delay_ms: int, p90: float) -> bool:
    """With async IO the TinyUSB task must not wait for the storage; without it, it must."""
    if delay_ms == 0:
        return True
    if io_mode == 'async':
        return p90 < delay_ms / 4
    # Guards the check itself: without async IO the storage delay must show up
    return p90 > delay_ms / 2


def check_latency(cdc: serial.Serial, args) -> bool:
    load_path = os.path.join(args.volume, 'MSCCHECK.BIN')
    delays = [int(d) for d in args.delays.split(',')]
    interval = 1.0 / args.probe_hz
    io_mode = get_io_mode(cdc)
    passed = True

    hdr = (f'{"delay":>5} {"phase":>5} | {"p50 ms":>8} {"p90 ms":>8} {"max ms":>8} {"n":>6} {"tmo":>4} |'
           f' {"wr MB/s":>8} {"rd MB/s":>8} {"rounds":>6} {"verify":>6} {"MSC rd/wr ops":>14} | {"result":>6}')
    print(f'io_mode={io_mode} file={args.file_kb} KiB, {args.duration:.0f} s per phase')
    print(hdr)
    print('-' * len(hdr))
    # Idle has no storage access, so the delay does not affect it: one baseline row is enough
    for phase, delay in [('idle', 0)] + [('load', d) for d in delays]:
        set_delay(cdc, delay)
        ctrl(cdc, b'R')
        lat, load = run_phase(cdc, args.duration, interval, args.timeout,
                              load_path if phase == 'load' else None, args.file_kb * 1024)
        reads, writes, _ = get_stats(cdc)
        s = lat.summary()
        if phase == 'idle':
            ok = s['n'] > 0
            load_cols = f' {"-":>8} {"-":>8} {"-":>6} {"-":>6}'
        else:
            wr = statistics.mean(load.write_mbps) if load.write_mbps else float('nan')
            rd = statistics.mean(load.read_mbps) if load.read_mbps else float('nan')
            verify = 'OK' if load.verify_fail == 0 and load.rounds else 'FAIL'
            ok = (verify == 'OK' and not load.error and writes > 0
                  and latency_ok(io_mode, delay, s['p90']))
            load_cols = f' {fmt(wr)} {fmt(rd)} {load.rounds:>6} {verify:>6}'
        passed = passed and ok
        print(f'{delay:>5} {phase:>5} | {fmt(s["p50"])} {fmt(s["p90"])} {fmt(s["max"])} {s["n"]:>6} {s["timeouts"]:>4} |'
              f'{load_cols} {f"{reads}/{writes}":>14} | {"PASS" if ok else "FAIL":>6}')
        if load.error:
            print(f'      load error: {load.error}')
    set_delay(cdc, 0)
    if os.path.exists(load_path):
        os.remove(load_path)
    print(f'Async IO worker stack (peak used/size): {get_worker_stack(cdc)}')
    return passed


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
        _drop_cache(fd)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
    return ''


def check_write_error(cdc: serial.Serial, volume: str, file_kb: int) -> bool:
    """Make one device-side write fail, report where the host notices it, and check for silent data loss."""
    io_mode = get_io_mode(cdc)
    set_delay(cdc, 0)
    set_fail_writes(cdc, 1)

    path = os.path.join(volume, 'MSCCHECK.BIN')
    data = os.urandom(file_kb * 1024)
    first = _write_file(path, data)
    # Read back before the next write overwrites the file: if the host saw no error,
    # a mismatch means the failed write was lost without the host knowing
    readback = ''
    if not first:
        try:
            readback = 'match' if _read_file(path) == data else 'MISMATCH'
        except OSError:
            readback = 'error'
    follow_up = _write_file(path, os.urandom(64 * 1024))
    set_fail_writes(cdc, 0)

    print(f'io_mode={io_mode}: 1 failing device write while writing {file_kb} KiB:')
    print(f'  same file : {"error at " + first if first else "no error"}')
    if readback:
        print(f'  read back : {readback}')
    print(f'  next write: {"error at " + follow_up if follow_up else "no error"}')
    print(f'Async IO worker stack (peak used/size): {get_worker_stack(cdc)}')
    try:
        os.remove(path)
    except OSError:
        pass

    reported = bool(first or follow_up or readback == 'error')
    if io_mode == 'async':
        if reported:
            print('PASS: the failed write reached the host')
        elif readback == 'MISMATCH':
            print('FAIL: the written data was lost and the host was NOT told')
        else:
            print('FAIL: the failed write was NOT reported to the host (it did not hit this file\'s data)')
        return reported
    # Without async IO, write errors are only logged on the device
    if readback == 'MISMATCH':
        print('PASS: sync IO, the error is only in the device log (data was lost, as expected without async IO)')
    else:
        print('PASS: sync IO, the error is only in the device log' if not reported else
              'NOTE: sync IO, but the host saw an error')
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True, help='CDC port of the device, e.g. /dev/cu.usbmodem* on macOS or /dev/ttyACM* on Linux')
    ap.add_argument('--volume', required=True, help='Mount point of the MSC drive, e.g. "/Volumes/NO NAME" or /media/$USER/XXXX-XXXX')
    ap.add_argument('--delays', default='0,5,20', help='Comma separated storage delays in ms (default: %(default)s)')
    ap.add_argument('--duration', type=float, default=5.0, help='Seconds per phase (default: %(default)s)')
    ap.add_argument('--file-kb', type=int, default=1024, help='Size of the file written/read under load (default: %(default)s)')
    ap.add_argument('--probe-hz', type=float, default=200.0, help='Echo probes per second (default: %(default)s)')
    ap.add_argument('--timeout', type=float, default=2.0, help='Echo timeout in seconds (default: %(default)s)')
    ap.add_argument('--inject-errors', action='store_true',
                    help='Skip the latency check; make one device write fail and check that the host sees it')
    args = ap.parse_args()

    if not os.path.ismount(args.volume):
        print(f'{args.volume} is not a mounted volume', file=sys.stderr)
        return 1

    cdc = serial.Serial(args.port, 115200, timeout=1.0)
    time.sleep(0.2)
    try:
        if args.inject_errors:
            passed = check_write_error(cdc, args.volume, args.file_kb)
        else:
            passed = check_latency(cdc, args)
    finally:
        cdc.close()
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
