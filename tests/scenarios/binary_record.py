#!/usr/bin/env python3
"""binary_record.py — synthetic fixed-layout binary records (custom schema).

Emits a deterministic stream of 32-byte records from a single logical device.
Each position has a distinct statistical character — the point of structural
landmarks:
  [0]      magic 0xAA           (constant)
  [1]      version 0x03         (constant)
  [2:4]    device id            (constant per stream)
  [4:8]    seq counter          (monotonic, small per-record delta)
  [8:12]   timestamp            (monotonic, small delta)
  [12:14]  sensor1              (slow random walk)
  [14:16]  sensor2              (slow random walk)
  [16:18]  sensor3              (slow random walk)
  [18:20]  voltage mV           (slow walk)
  [20]     status flags         (mostly constant)
  [21:24]  reserved             (zeros, constant)
  [24:28]  uptime counter       (monotonic)
  [28:32]  crc32-ish            (high entropy)

Usage: binary_record.py [--bytes N] [--seed S]
"""
import argparse
import random
import struct
import sys

REC = 32


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", type=int, default=262144)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = sys.stdout.buffer

    dev = rng.randint(0x1000, 0x1FFF)
    seq = rng.randint(0, 1000)
    ts = 1_700_000_000 + rng.randint(0, 86400)
    s1, s2, s3 = rng.randint(2000, 3000), rng.randint(3000, 7000), rng.randint(100, 900)
    volt = rng.randint(3300, 4200)
    status = 0x01
    uptime = rng.randint(0, 100000)

    written = 0
    while written < args.bytes:
        seq = (seq + 1) & 0xFFFFFFFF
        ts += rng.randint(1, 4)
        s1 = max(1500, min(3500, s1 + rng.randint(-4, 4)))
        s2 = max(2000, min(9000, s2 + rng.randint(-6, 6)))
        s3 = max(0, min(1023, s3 + rng.randint(-3, 3)))
        volt = max(3000, min(4200, volt - (1 if rng.random() < 0.1 else 0)))
        if rng.random() < 0.03:
            status ^= 0x02
        uptime = (uptime + 1) & 0xFFFFFFFF
        rec = struct.pack(">BBHIIHHHHB3sI I",
                          0xAA, 0x03, dev, seq, ts & 0xFFFFFFFF,
                          s1, s2, s3, volt, status, b"\x00\x00\x00",
                          uptime, rng.getrandbits(32))
        out.write(rec[:REC])
        written += REC


if __name__ == "__main__":
    main()
