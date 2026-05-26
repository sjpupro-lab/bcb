#!/usr/bin/env python3
"""check_regression.py — CI guard for small-message compression ratios.

Runs bcb-msgbench over every scenario corpus, parses the BCB compression
ratio for each (scenario, message_size), and compares against a committed
baseline. Fails (exit 1) if any BCB ratio regresses by more than --tol
(fractional, e.g. 0.02 = 2%). Improvements never fail.

Use --update to (re)write the baseline from the current measurement.
"""
import argparse
import json
import os
import subprocess
import sys

SCENARIOS = ["http_headers", "iot_packets", "mqtt_messages", "log_lines", "rpc_calls"]


def run_msgbench(binpath, corpus, train_size, sizes, samples):
    """Return {msg_size: bcb_ratio} parsed from --md output."""
    out = subprocess.check_output([
        binpath, "--corpus", corpus, "--train-size", str(train_size),
        "--message-sizes", sizes, "--samples", str(samples), "--md",
    ], text=True)
    ratios = {}
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("|") or "msg_size" in line or "---" in line:
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        # | msg_size | BCB(B) | BCB(x) | brotli(B) | brotli(x) | zstd(B) | zstd(x) | winner |
        if len(cells) < 3:
            continue
        try:
            ratios[int(cells[0])] = float(cells[2])
        except ValueError:
            continue
    return ratios


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--build", required=True)
    ap.add_argument("--train-size", type=int, default=50000)
    ap.add_argument("--message-sizes", default="64,128,256,512,1024,2048,4096")
    ap.add_argument("--samples", type=int, default=64)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--tol", type=float, default=0.02)
    ap.add_argument("--update", action="store_true")
    args = ap.parse_args()

    measured = {}
    for s in SCENARIOS:
        corpus = os.path.join(args.build, "corpus_%s.bin" % s)
        measured[s] = run_msgbench(args.bin, corpus, args.train_size,
                                   args.message_sizes, args.samples)

    if args.update:
        payload = {
            "train_size": args.train_size,
            "message_sizes": args.message_sizes,
            "samples": args.samples,
            "bcb_ratio": {s: {str(k): round(v, 3) for k, v in measured[s].items()}
                          for s in SCENARIOS},
        }
        with open(args.baseline, "w") as f:
            json.dump(payload, f, indent=2)
            f.write("\n")
        print("baseline written to", args.baseline)
        return 0

    with open(args.baseline) as f:
        base = json.load(f)
    base_ratio = base["bcb_ratio"]

    failures = []
    print("%-16s %8s %10s %10s %8s" % ("scenario", "msg", "baseline", "measured", "delta%"))
    for s in SCENARIOS:
        for k, mv in sorted(measured[s].items()):
            bv = base_ratio.get(s, {}).get(str(k))
            if bv is None:
                print("%-16s %8d %10s %10.2f   (new)" % (s, k, "-", mv))
                continue
            delta = (mv - bv) / bv
            flag = ""
            if delta < -args.tol:
                flag = "  FAIL"
                failures.append((s, k, bv, mv, delta))
            print("%-16s %8d %10.2f %10.2f %+7.2f%%%s" % (s, k, bv, mv, delta * 100, flag))

    if failures:
        print("\n%d ratio regression(s) beyond %.1f%%:" % (len(failures), args.tol * 100))
        for s, k, bv, mv, d in failures:
            print("  %s @%dB: %.2fx -> %.2fx (%+.2f%%)" % (s, k, bv, mv, d * 100))
        return 1
    print("\nOK: all BCB ratios within %.1f%% of baseline." % (args.tol * 100))
    return 0


if __name__ == "__main__":
    sys.exit(main())
