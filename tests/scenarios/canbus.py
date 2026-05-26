#!/usr/bin/env python3
"""canbus.py — synthetic CAN 2.0 bus traffic (fixed 16-byte log records).

Realistic interleaved automotive bus: a handful of recurring CAN IDs, each with
its own fixed signal layout in the 8 data bytes. Logged as fixed 16-byte
records (the common "candump"-style fixed framing):
  [0:4]   CAN id (29-bit, big-endian)   — cycles through a few IDs
  [4]     DLC (data length, =8)
  [5:13]  data (8 bytes; per-id signals: counters + slow-varying)
  [13:16] reserved (zeros)

Because IDs are interleaved, position p means different signals for different
IDs — an honest stress test for position-aware vs per-stream structure.

Usage: canbus.py [--bytes N] [--seed S]
"""
import argparse
import random
import struct
import sys

REC = 16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", type=int, default=262144)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = sys.stdout.buffer

    ids = [0x0C0, 0x1A0, 0x2B5, 0x3E8, 0x420]
    # per-id signal state: [counter, rpm-ish, temp-ish, byte counter]
    state = {i: [rng.randint(0, 255), rng.randint(0, 8000), rng.randint(60, 110), 0] for i in ids}
    order = 0

    written = 0
    while written < args.bytes:
        cid = ids[order % len(ids)]
        order += 1
        st = state[cid]
        st[0] = (st[0] + 1) & 0xFF                       # rolling counter
        st[1] = max(0, min(8000, st[1] + rng.randint(-50, 50)))   # rpm
        st[2] = max(0, min(200, st[2] + rng.randint(-1, 1)))      # temp (offset-encoded)
        st[3] = (st[3] + 1) & 0x0F
        data = struct.pack(">BHBBBBB", st[0], st[1], st[2], st[3],
                           rng.getrandbits(8) if rng.random() < 0.2 else 0x00, 0x00, 0x00)
        rec = struct.pack(">IB", cid, 8) + data + b"\x00\x00\x00"
        out.write(rec[:REC])
        written += REC


if __name__ == "__main__":
    main()
