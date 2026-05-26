#!/usr/bin/env python3
"""landmark_bench.py — PR-3 benchmark: base vs landmark prior.

For each scenario and message size, builds a base prior and a landmark prior,
chunks the test region into fixed-size blocks, and measures (actual coded
bytes, via tools/bcb-blockbench):
  * compression ratio   (base vs landmark)
  * throughput          (blocks/s, base vs landmark)
  * per-message latency  (ms/block at 64B = cold-start proxy)
  * message-level random access (decode arbitrary blocks standalone)

Usage: python3 tools/landmark_bench.py [--build build] [--train 50000]
                                       [--k 512] [--n 8] [--write-docs PATH]
"""
import argparse
import os
import re
import subprocess
import sys

SCN = ["http_headers", "mqtt_messages", "log_lines", "iot_packets"]
SIZES = [64, 128, 256, 512]
SUMMARY_RE = re.compile(r"lossless=(\w+).*ratio=([\d.]+).*blocks_per_s=([\d.]+)")
RAND_RE = re.compile(r"random-access: decoded (\d+).*correct=(\w+), avg=([\d.]+) us")


def run_blockbench(binp, prior, blocks, random_n=0):
    cmd = [binp, "--prior", prior, "--blocks", blocks]
    if random_n:
        cmd += ["--random", str(random_n)]
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    m = SUMMARY_RE.search(r.stderr)
    out = {"lossless": m.group(1), "ratio": float(m.group(2)), "bps": float(m.group(3))} if m else None
    rm = RAND_RE.search(r.stderr)
    if rm:
        out["rand_correct"] = rm.group(2)
        out["rand_us"] = float(rm.group(3))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--train", type=int, default=50000)
    ap.add_argument("--k", type=int, default=512)
    ap.add_argument("--n", type=int, default=8)
    ap.add_argument("--max", type=int, default=2000)
    ap.add_argument("--write-docs")
    args = ap.parse_args()
    b = args.build
    here = os.path.dirname(__file__)
    chunk = os.path.join(here, "..", "tests", "scenarios", "chunk.py")
    bb = os.path.join(b, "bcb-blockbench")
    build_tool = os.path.join(b, "bcb-prior-build")

    rows = []   # (scenario, size, base, land)
    rand_us = {}
    for s in SCN:
        corpus = os.path.join(b, "corpus_%s.bin" % s)
        base_p = os.path.join(b, "%s.base.prior" % s)
        land_p = os.path.join(b, "%s.lm.prior" % s)
        subprocess.check_call([build_tool, corpus, base_p, "--train-size", str(args.train)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.check_call([build_tool, corpus, land_p, "--train-size", str(args.train),
                               "--landmark-n", str(args.n), "--landmark-k", str(args.k)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for sz in SIZES:
            blocks = os.path.join(b, "%s.%d.blocks" % (s, sz))
            subprocess.check_call([sys.executable, chunk, corpus, "--train-size", str(args.train),
                                   "--size", str(sz), "--out", blocks, "--max", str(args.max)],
                                  stdout=subprocess.DEVNULL)
            base = run_blockbench(bb, base_p, blocks)
            rnd = args.max if sz == 64 else 0     # random-access demo on 64B set
            land = run_blockbench(bb, land_p, blocks, random_n=rnd)
            rows.append((s, sz, base, land))
            if sz == 64 and land and "rand_us" in land:
                rand_us[s] = (land["rand_correct"], land["rand_us"])

    # report
    print("%-15s %5s | %8s %8s %6s | %8s %8s %6s | %s" % (
        "scenario", "size", "base x", "land x", "gain%", "base b/s", "land b/s", "spd", "lossless"))
    for s, sz, base, land in rows:
        if not base or not land:
            continue
        gain = 100 * (land["ratio"] - base["ratio"]) / base["ratio"]
        spd = land["bps"] / base["bps"] if base["bps"] else 0
        ll = "yes" if base["lossless"] == "yes" and land["lossless"] == "yes" else "NO"
        print("%-15s %5d | %8.2f %8.2f %+6.1f | %8.0f %8.0f %5.1fx | %s" % (
            s, sz, base["ratio"], land["ratio"], gain, base["bps"], land["bps"], spd, ll))
    print("\nmessage-level random access (64B sets):")
    for s, (ok, us) in rand_us.items():
        print("  %-15s decode arbitrary block standalone: correct=%s, %.1f us/block" % (s, ok, us))

    if args.write_docs:
        write_docs(args.write_docs, rows, rand_us, args)
        sys.stderr.write("wrote %s\n" % args.write_docs)


def write_docs(path, rows, rand_us, args):
    lines = ["## PR-3 — 측정 + 벤치마크 (base vs landmark)\n",
             "`make landmark-bench` 실측 (train %dKB, K=%d, N=%d, 최대 %d블록/크기, 실제 코딩 바이트)." % (
                 args.train // 1000, args.k, args.n, args.max), "",
             "| 시나리오 | 크기 | base× | land× | 압축 이득 | base b/s | land b/s | 속도 | lossless |",
             "|---|---|---|---|---|---|---|---|---|"]
    for s, sz, base, land in rows:
        if not base or not land:
            continue
        gain = 100 * (land["ratio"] - base["ratio"]) / base["ratio"]
        spd = land["bps"] / base["bps"] if base["bps"] else 0
        ll = "yes" if base["lossless"] == "yes" and land["lossless"] == "yes" else "NO"
        lines.append("| %s | %d | %.2f | %.2f | %+.1f%% | %.0f | %.0f | %.1f× | %s |" % (
            s, sz, base["ratio"], land["ratio"], gain, base["bps"], land["bps"], spd, ll))
    lines += ["",
              "### Cold-start / per-message latency (64B)",
              "위 표의 base/land b/s 역수가 메시지당 인코딩 지연이다. 64B http: "
              "base ~%.0f us → land ~%.0f us/msg." % tuple(
                  [1e6 / r[2]["bps"] for r in rows if r[0] == "http_headers" and r[1] == 64][:1]
                  + [1e6 / r[3]["bps"] for r in rows if r[0] == "http_headers" and r[1] == 64][:1]
              ) if any(r[0] == "http_headers" and r[1] == 64 for r in rows) else "(n/a)",
              "mmap prior 로드는 즉시(별도: `docs/mmap_prior.md`) — cold-start 는 사실상 첫 메시지 지연이다.",
              "",
              "### Message-level random access",
              "각 메시지는 prior 로 **독립** 압축되므로, offset index 만 있으면 임의 메시지를 "
              "다른 메시지 디코드 없이 standalone 복원한다 (`bcb-blockbench --random`):"]
    for s, (ok, us) in rand_us.items():
        lines.append("- %s: 임의 블록 standalone 복원 correct=%s, **%.1f us/block** (위치 무관)." % (s, ok, us))
    lines += ["",
              "> 한계(정직): **sub-message** random access 는 불가하다. range coder 는 순차적이라 "
              "메시지 내 N번째 바이트는 0..N-1 을 디코드해야 얻는다. landmark 도 이를 바꾸지 않는다. "
              "random access 는 **메시지 단위**에서만 성립한다 (각 메시지가 독립 압축이라 가능).", ""]
    with open(path, "a", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
