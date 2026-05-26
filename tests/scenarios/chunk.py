#!/usr/bin/env python3
"""chunk.py — slice a corpus's test region into length-delimited blocks.

Writes [u32 LE len][bytes] repeated, for tools/bcb-blockbench. The test region
is everything after --train-size (so blocks are disjoint from the prior's
training data). Used by the landmark verification harness.

Usage: chunk.py <corpus> --train-size N --size S --out <file> [--max M]
"""
import argparse
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--train-size", type=int, default=50000)
    ap.add_argument("--size", type=int, default=128)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max", type=int, default=2000)
    args = ap.parse_args()

    data = open(args.corpus, "rb").read()
    test = data[args.train_size:]
    n = min(len(test) // args.size, args.max)
    with open(args.out, "wb") as f:
        for i in range(n):
            blk = test[i * args.size:(i + 1) * args.size]
            f.write(struct.pack("<I", len(blk)))
            f.write(blk)
    print("%d blocks of %dB -> %s" % (n, args.size, args.out))


if __name__ == "__main__":
    main()
