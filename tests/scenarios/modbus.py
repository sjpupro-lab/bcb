#!/usr/bin/env python3
"""modbus.py — synthetic Modbus/TCP read-holding-registers responses.

Fixed 25-byte records (one connection, function 0x03, 8 registers):
  [0:2]   transaction id   (monotonic per request)
  [2:4]   protocol id      (constant 0x0000)
  [4:6]   length           (constant 0x0013)
  [6]     unit id          (constant)
  [7]     function code    (constant 0x03)
  [8]     byte count       (constant 0x10 = 16)
  [9:25]  8x 16-bit registers (slow random walks — sensor/holding values)

Industrial telemetry: heavy positional structure, slowly-varying registers.

Usage: modbus.py [--bytes N] [--seed S]
"""
import argparse
import random
import struct
import sys

REC = 25
NREG = 8


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", type=int, default=262144)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = sys.stdout.buffer

    txn = rng.randint(0, 5000)
    unit = rng.randint(1, 8)
    regs = [rng.randint(0, 4000) for _ in range(NREG)]

    written = 0
    while written < args.bytes:
        txn = (txn + 1) & 0xFFFF
        for i in range(NREG):
            regs[i] = max(0, min(65535, regs[i] + rng.randint(-5, 5)))
        rec = struct.pack(">HHHBBB", txn, 0x0000, 0x0013, unit, 0x03, 0x10)
        rec += b"".join(struct.pack(">H", r) for r in regs)
        out.write(rec[:REC])
        written += REC


if __name__ == "__main__":
    main()
