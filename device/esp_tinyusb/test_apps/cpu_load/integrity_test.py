#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# Test has been generated with AI
# Verified by Roman Leonov manually

"""
CDC Echo Integrity Tester

Sends randomly generated data to a CDC-ACM echo device, reads back the echoed
bytes, and verifies integrity by comparing a hash (SHA or HMAC) of the
transmitted payload versus the received payload.

Usage examples:

  # Select port explicitly and send 4 MiB in 1 KiB chunks using SHA-256
  python cdc_echo_integrity.py --port COM7 --bytes 4MiB --chunk-size 1KiB

  # Auto-select by VID:PID (hex) and use HMAC-SHA256 with a key
  python cdc_echo_integrity.py --vid 0x303A --pid 0x1001 \
      --bytes 2MiB --hmac-key secretkey

  # Use SHA-512 and deterministic pseudo-random data with seed
  python cdc_echo_integrity.py --port /dev/ttyACM0 --algo sha512 --seed 1234

Exit code 0 on success, non-zero on failure.
"""
from __future__ import annotations

import argparse
import binascii
import os
import sys
import time
import hashlib
import hmac
from dataclasses import dataclass
from typing import Optional, Callable, Tuple

try:
    import serial  # pyserial
    from serial.tools import list_ports
except Exception as e:  # pragma: no cover - import guidance
    print("This script requires pyserial. Install with: pip install pyserial", file=sys.stderr)
    raise

# ------------------------------- Helpers ------------------------------------

SIZE_SUFFIXES = {
    "b": 1,
    "kb": 1000,
    "kib": 1024,
    "mb": 1000 ** 2,
    "mib": 1024 ** 2,
    "gb": 1000 ** 3,
    "gib": 1024 ** 3,
}

def parse_size(s: str) -> int:
    """Parse human-readable size like 4096, 4KiB, 1MB, 2MiB."""
    s = s.strip().lower()
    # plain int
    if s.isdigit():
        return int(s)
    # try suffixes
    for suf in sorted(SIZE_SUFFIXES, key=len, reverse=True):
        if s.endswith(suf):
            num = s[: -len(suf)].strip()
            return int(float(num) * SIZE_SUFFIXES[suf])
    # accept hex
    if s.startswith("0x"):
        return int(s, 16)
    raise argparse.ArgumentTypeError(f"Invalid size: {s}")


def find_port(vid: Optional[int], pid: Optional[int], explicit_port: Optional[str]) -> str:
    if explicit_port:
        return explicit_port
    if vid is None and pid is None:
        raise SystemExit("Either --port or both --vid and --pid must be specified")
    candidates = []
    for p in list_ports.comports():
        if p.vid is None or p.pid is None:
            continue
        if vid is not None and pid is not None and p.vid == vid and p.pid == pid:
            candidates.append(p.device)
    if not candidates:
        raise SystemExit(f"No port found for VID:PID {vid:#06x}:{pid:#06x}")
    if len(candidates) > 1:
        print("Multiple matching ports found; using first:", candidates, file=sys.stderr)
    return candidates[0]


def make_hashers(algo: str, hmac_key: Optional[bytes]) -> Tuple[Callable[[bytes], None], Callable[[bytes], None], Callable[[], str]]:
    """Return (tx_update, rx_update, hexdigest) closures configured per algo/mode."""
    algo = algo.lower()
    if hmac_key is not None:
        # HMAC mode
        try:
            tx_h = hmac.new(hmac_key, digestmod=algo)
            rx_h = hmac.new(hmac_key, digestmod=algo)
        except ValueError:
            raise SystemExit(f"Unsupported HMAC algorithm: {algo}")
        return tx_h.update, rx_h.update, lambda: (tx_h.hexdigest(), rx_h.hexdigest())
    # Plain hash mode
    try:
        tx_h = hashlib.new(algo)
        rx_h = hashlib.new(algo)
    except ValueError:
        raise SystemExit(f"Unsupported hash algorithm: {algo}")
    return tx_h.update, rx_h.update, lambda: (tx_h.hexdigest(), rx_h.hexdigest())


def iter_chunks(buf: bytes, chunk_size: int):
    for i in range(0, len(buf), chunk_size):
        yield memoryview(buf)[i : i + chunk_size]


@dataclass
class Stats:
    total_written: int = 0
    total_read: int = 0
    start_time: float = 0.0

    def start(self):
        self.start_time = time.perf_counter()

    @property
    def elapsed(self) -> float:
        return max(1e-9, time.perf_counter() - self.start_time)

    @property
    def write_mbps(self) -> float:
        return (self.total_written * 8) / (1_000_000 * self.elapsed)

    @property
    def read_mbps(self) -> float:
        return (self.total_read * 8) / (1_000_000 * self.elapsed)


# ------------------------------- Core ---------------------------------------

def generate_payload(nbytes: int, seed: Optional[int]) -> bytes:
    if seed is None:
        return os.urandom(nbytes)
    # Deterministic pseudo-random: use a simple stream based on hashlib
    out = bytearray()
    counter = 0
    while len(out) < nbytes:
        counter_bytes = counter.to_bytes(8, 'little')
        block = hashlib.sha256(seed.to_bytes(8, 'little') + counter_bytes).digest()
        out.extend(block)
        counter += 1
    return bytes(out[:nbytes])


def echo_test(
    port: str,
    nbytes: int,
    chunk_size: int,
    algo: str,
    hmac_key: Optional[bytes],
    baudrate: int,
    timeout: float,
    seed: Optional[int],
    rtscts: bool,
    dsrdtr: bool,
    xonxoff: bool,
) -> bool:
    payload = generate_payload(nbytes, seed)
    tx_update, rx_update, hexdigests = make_hashers(algo, hmac_key)

    stats = Stats()

    with serial.Serial(
        port=port,
        baudrate=baudrate,
        timeout=timeout,
        rtscts=rtscts,
        dsrdtr=dsrdtr,
        xonxoff=xonxoff,
        write_timeout=timeout,
    ) as set:
        # Flush any stale bytes
        set.reset_input_buffer()
        set.reset_output_buffer()

        stats.start()
        # Write in chunks to avoid huge buffers
        for chunk in iter_chunks(payload, chunk_size):
            n = set.write(chunk)
            if n != len(chunk):
                raise SystemExit(f"Short write: wrote {n} of {len(chunk)} bytes")
            stats.total_written += n
            tx_update(chunk)

        # Read back exactly nbytes
        received = bytearray()
        while len(received) < nbytes:
            part = set.read(min(chunk_size, nbytes - len(received)))
            if not part:
                raise SystemExit(
                    f"Timeout while reading echoed data: got {len(received)}/{nbytes} bytes"
                )
            received.extend(part)
            stats.total_read += len(part)
            rx_update(part)

    tx_hex, rx_hex = hexdigests()

    ok = (len(received) == len(payload)) and (tx_hex == rx_hex)

    print("=== CDC Echo Integrity Test ===")
    print(f"Port           : {port}")
    print(f"Bytes          : {nbytes}")
    print(f"Chunk size     : {chunk_size}")
    print(f"Mode           : {'HMAC' if hmac_key else 'Hash'} ({algo})")
    print(f"Elapsed (s)    : {stats.elapsed:.3f}")
    print(f"TX rate (Mb/s) : {stats.write_mbps:.3f}")
    print(f"RX rate (Mb/s) : {stats.read_mbps:.3f}")
    print(f"TX digest      : {tx_hex}")
    print(f"RX digest      : {rx_hex}")
    print(f"Result         : {'PASS' if ok else 'FAIL'}")

    return ok


# ------------------------------- CLI ----------------------------------------

def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="CDC echo integrity tester (SHA or HMAC)")

    g_dev = p.add_argument_group("Device selection")
    g_dev.add_argument("--port", help="Serial port name, e.g., COM7 or /dev/ttyACM0")
    g_dev.add_argument("--vid", type=lambda s: int(s, 0), help="USB VID in hex, e.g., 0x303A")
    g_dev.add_argument("--pid", type=lambda s: int(s, 0), help="USB PID in hex, e.g., 0x1001")

    g_io = p.add_argument_group("I/O settings")
    g_io.add_argument("--baudrate", type=int, default=115200, help="Baudrate (some CDC stacks ignore this)")
    g_io.add_argument("--timeout", type=float, default=5.0, help="Read/write timeout in seconds")
    g_io.add_argument("--rtscts", action="store_true", help="Enable RTS/CTS flow control")
    g_io.add_argument("--dsrdtr", action="store_true", help="Enable DSR/DTR flow control")
    g_io.add_argument("--xonxoff", action="store_true", help="Enable XON/XOFF software flow control")

    g_data = p.add_argument_group("Test payload")
    g_data.add_argument("--bytes", type=parse_size, default="1MiB", help="Total payload size (e.g., 1MiB, 4096)")
    g_data.add_argument("--chunk-size", type=parse_size, default="512", help="Chunk size for TX/RX")
    g_data.add_argument("--seed", type=int, help="Deterministic pseudo-random payload seed (optional)")

    g_hash = p.add_argument_group("Integrity")
    g_hash.add_argument("--algo", default="sha256", help="Hash algorithm (e.g., sha256, sha1, sha512)")
    g_hash.add_argument(
        "--hmac-key",
        help="If provided, compute HMAC instead of plain hash. Accepts ASCII or hex with 0x prefix.",
    )

    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)

    port = find_port(args.vid if hasattr(args, 'vid') else None,
                     args.pid if hasattr(args, 'pid') else None,
                     args.port if hasattr(args, 'port') else None)

    key_bytes: Optional[bytes] = None
    if args.hmac_key:
        if args.hmac_key.startswith("0x"):
            try:
                key_bytes = binascii.unhexlify(args.hmac_key[2:])
            except binascii.Error:
                raise SystemExit("Invalid hex for --hmac-key")
        else:
            key_bytes = args.hmac_key.encode("utf-8")

    ok = echo_test(
        port=port,
        nbytes=args.bytes,
        chunk_size=args.chunk_size,
        algo=args.algo,
        hmac_key=key_bytes,
        baudrate=args.baudrate,
        timeout=args.timeout,
        seed=args.seed,
        rtscts=args.rtscts,
        dsrdtr=args.dsrdtr,
        xonxoff=args.xonxoff,
    )

    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
