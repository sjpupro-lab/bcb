#!/usr/bin/env python3
"""bcb_vs_hpack.py — BCB (shared prior) vs HPACK (RFC 7541) on HTTP header blocks.

Compares, on the same header blocks and the same shared history:
  * BCB    : a frozen .bcb-prior trained on the history; each block compressed
             independently (stateless).
  * HPACK  : reference RFC 7541 (pip 'hpack'), in two regimes —
       cold : fresh encoder per block (first request of a connection; static
              table + Huffman only, no dynamic-table benefit).
       warm : one encoder per direction, pre-fed the training history, then
              encoding test blocks statefully (dynamic table populated — both
              methods have seen the same prior traffic).

Original (uncompressed) size = the textual "name: value\\r\\n"... serialization,
the same baseline for both compressors' ratios.

Usage:
  python3 tools/bcb_vs_hpack.py [--blocks-file blocks.jsonl] [--train-bytes 50000]
                                [--bcb-build build] [--write-docs docs/hpack_comparison.md]
If --blocks-file is omitted, runs tests/scenarios/http2_headers.py.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import time

try:
    from hpack import Encoder
except ImportError:
    sys.stderr.write("missing 'hpack' (pip install hpack)\n")
    sys.exit(2)


def serialize(headers):
    return ("".join("%s: %s\r\n" % (n, v) for n, v in headers) + "\r\n").encode("utf-8")


def load_blocks(args):
    if args.blocks_file:
        lines = open(args.blocks_file, encoding="utf-8").read().splitlines()
    else:
        gen = os.path.join(os.path.dirname(__file__), "..", "tests", "scenarios", "http2_headers.py")
        out = subprocess.check_output([sys.executable, gen, "--blocks", str(args.gen_blocks)], text=True)
        lines = out.splitlines()
    return [json.loads(l) for l in lines if l.strip()]


def hpack_size(enc, headers):
    return len(enc.encode(headers))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--blocks-file")
    ap.add_argument("--gen-blocks", type=int, default=3000)
    ap.add_argument("--train-bytes", type=int, default=50000)
    ap.add_argument("--bcb-build", default="build")
    ap.add_argument("--write-docs")
    args = ap.parse_args()

    blocks = load_blocks(args)
    ser = [serialize(b["headers"]) for b in blocks]

    # split: training history (~train-bytes) vs test
    cum, train_n = 0, 0
    for i, s in enumerate(ser):
        cum += len(s)
        train_n = i + 1
        if cum >= args.train_bytes:
            break
    test = list(range(train_n, len(blocks)))
    if not test:
        sys.stderr.write("not enough blocks for a test split\n")
        return 2

    build = args.bcb_build
    corpus_path = os.path.join(build, "hpack_train.bin")
    blocks_path = os.path.join(build, "hpack_test.blocks")
    prior_path = os.path.join(build, "hpack.bcb-prior")
    os.makedirs(build, exist_ok=True)

    with open(corpus_path, "wb") as f:
        for i in range(train_n):
            f.write(ser[i])
    with open(blocks_path, "wb") as f:
        for i in test:
            f.write(struct.pack("<I", len(ser[i])))
            f.write(ser[i])

    # build BCB prior on the training history
    subprocess.check_call([os.path.join(build, "bcb-prior-build"), corpus_path, prior_path],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # BCB per-block sizes via the frozen mmap prior
    t0 = time.monotonic()
    res = subprocess.run([os.path.join(build, "bcb-blockbench"),
                          "--prior", prior_path, "--blocks", blocks_path],
                         capture_output=True, text=True)
    bcb_wall = time.monotonic() - t0
    if res.returncode not in (0,):
        sys.stderr.write("bcb-blockbench failed: %s\n" % res.stderr)
        return 1
    bcb_lines = [l for l in res.stdout.splitlines() if l.strip()]
    bcb = [int(l.split()[1]) for l in bcb_lines]   # comp size per test block
    bcb_summary = res.stderr.strip()

    # HPACK cold: fresh encoder per block
    t0 = time.monotonic()
    hp_cold = [hpack_size(Encoder(), blocks[i]["headers"]) for i in test]
    cold_wall = time.monotonic() - t0

    # HPACK warm: per-direction encoders pre-fed the training history, then test
    enc_req, enc_resp = Encoder(), Encoder()
    for i in range(train_n):
        (enc_req if blocks[i]["type"] == "req" else enc_resp).encode(blocks[i]["headers"])
    t0 = time.monotonic()
    hp_warm = []
    for i in test:
        e = enc_req if blocks[i]["type"] == "req" else enc_resp
        hp_warm.append(hpack_size(e, blocks[i]["headers"]))
    warm_wall = time.monotonic() - t0

    orig = [len(ser[i]) for i in test]
    types = [blocks[i]["type"] for i in test]

    def agg(idxs):
        o = sum(orig[j] for j in idxs)
        b = sum(bcb[j] for j in idxs)
        c = sum(hp_cold[j] for j in idxs)
        w = sum(hp_warm[j] for j in idxs)
        n = len(idxs)
        return dict(n=n, avg=o / n, orig=o,
                    bcb=b, bcb_x=o / b, bcb_avg=b / n,
                    cold=c, cold_x=o / c, cold_avg=c / n,
                    warm=w, warm_x=o / w, warm_avg=w / n)

    all_i = list(range(len(test)))
    req_i = [j for j in all_i if types[j] == "req"]
    resp_i = [j for j in all_i if types[j] == "resp"]
    rows = [("all", agg(all_i)), ("request", agg(req_i)), ("response", agg(resp_i))]

    print("BCB (shared prior, stateless per block) vs HPACK (RFC 7541)")
    print("train history: %d blocks (%d B), test: %d blocks\n" % (train_n, cum, len(test)))
    hdr = "%-9s %5s %7s | %8s %7s | %9s %8s | %9s %8s"
    print(hdr % ("category", "n", "avg(B)", "BCB(B)", "BCB(x)",
                 "HPK-cold", "cold(x)", "HPK-warm", "warm(x)"))
    for name, a in rows:
        print("%-9s %5d %7.1f | %8.1f %7.2f | %9.1f %8.2f | %9.1f %8.2f" % (
            name, a["n"], a["avg"], a["bcb_avg"], a["bcb_x"],
            a["cold_avg"], a["cold_x"], a["warm_avg"], a["warm_x"]))
    print("\nBCB:", bcb_summary)
    print("timing(wall): BCB(incl. proc) %.3fs, HPACK-cold %.3fs, HPACK-warm %.3fs (Python vs C — indicative)"
          % (bcb_wall, cold_wall, warm_wall))

    if args.write_docs:
        write_docs(args.write_docs, train_n, cum, len(test), rows, bcb_summary,
                   bcb_wall, cold_wall, warm_wall)
        sys.stderr.write("wrote %s\n" % args.write_docs)
    return 0


def write_docs(path, train_n, train_b, test_n, rows, bcb_summary, bw, cw, ww):
    def tbl(rows):
        out = ["| category | n | avg(B) | BCB(B) | BCB(×) | HPACK-cold(B) | cold(×) | HPACK-warm(B) | warm(×) |",
               "|---|---|---|---|---|---|---|---|---|"]
        for name, a in rows:
            out.append("| %s | %d | %.1f | %.1f | %.2f | %.1f | %.2f | %.1f | %.2f |" % (
                name, a["n"], a["avg"], a["bcb_avg"], a["bcb_x"],
                a["cold_avg"], a["cold_x"], a["warm_avg"], a["warm_x"]))
        return "\n".join(out)

    rd = dict(rows)
    a = rd["all"]
    cold_verdict = "BCB" if a["bcb_avg"] < a["cold_avg"] else "HPACK"
    warm_verdict = "BCB" if a["bcb_avg"] < a["warm_avg"] else "HPACK"
    resp = rd.get("response", a)
    resp_warm_note = ""
    if resp["bcb_avg"] < resp["warm_avg"]:
        resp_warm_note = ("- **response 헤더**: warm 에서도 BCB(%.2f×)가 HPACK-warm(%.2f×)을 앞선다 — "
                          "응답 헤더는 date/content-length/etag 등 가변 필드가 많아 동적 테이블 인덱싱이 "
                          "약하고, BCB 의 prior 가 이를 더 잘 다룬다.\n"
                          % (resp["bcb_x"], resp["warm_x"]))
    body = """# BCB vs HPACK — HTTP/2 헤더 압축 비교

`tools/bcb_vs_hpack.py` 실측. 재현: `make hpack` (또는 실제 트래픽: `http2_real.py capture.har`).

## 방법

- 같은 헤더 블록 시퀀스에서 BCB 와 HPACK(RFC 7541, pip `hpack`)을 비교한다.
- **원본 크기** = 텍스트 직렬화(`name: value\\r\\n`…) 바이트 — 두 압축기 ratio 의 공통 기준.
- **BCB**: 학습 이력(history)으로 만든 frozen `.bcb-prior`. 각 블록을 **독립**(stateless) 압축.
- **HPACK** 2 regime:
  - **cold** — 블록마다 새 encoder (연결 첫 요청; static table + Huffman, 동적 테이블 이점 없음).
  - **warm** — 방향별 encoder 를 학습 이력으로 미리 채운 뒤 test 블록을 **stateful** 압축
    (동적 테이블 populated — 양쪽이 같은 과거 트래픽을 본 가장 공정한 비교).

학습 이력 {train_n} 블록({train_b} B), test {test_n} 블록.

## 결과

{table}

(B=블록당 평균 압축 바이트, ×=원본/압축. BCB summary: {bcb_summary})

처리 시간(wall, 참고): BCB {bw:.3f}s(프로세스 포함, C), HPACK-cold {cw:.3f}s, HPACK-warm {ww:.3f}s (Python).
Python HPACK 과 C BCB 의 시간은 언어가 달라 직접 비교 불가 — 압축비가 핵심 지표다.

## 해석 (정직하게)

- **cold-start**: 동적 테이블이 비어 HPACK 은 static table(61개) + Huffman 만 쓴다. 학습 prior 를
  가진 BCB 가 이 구간에서 {cold_verdict} 우세. 연결 첫 요청·짧은 연결(HTTP/3 0-RTT, 모바일 재연결)에서 의미.
- **warm (steady state)**: HPACK 의 동적 테이블이 반복 헤더를 인덱스 1~2 바이트로 줄인다.
  stateless BCB 는 매 블록 prior 비용을 다시 치르므로 이 구간은 전체적으로 {warm_verdict} 우세.
{resp_warm_note}- 즉 BCB 의 기회는 **stateful 동적 테이블을 유지할 수 없는/원치 않는 상황**(연결당 1~수 요청,
  무상태 게이트웨이, 동적 테이블 메모리/HOL-blocking 회피)이다. 장수 연결의 반복 request 트래픽은 HPACK 영역.
- **비용**: BCB 는 블록당 range-coder 연산이 무거워 HPACK 보다 CPU 가 많이 든다(위 blocks/s 참고).
  압축비 이득은 처리량과 trade-off 다 — 헤더가 작고 연결이 짧을수록 BCB 가 합리적.

## HTTP/3 (QPACK) 후보성

QPACK 은 HOL-blocking 때문에 동적 테이블 사용을 보수적으로 제한한다(많은 구현이 static-only
또는 제한적 dynamic). **동적 테이블을 끄거나 줄인 QPACK ≈ HPACK-cold** 에 가깝고, 그 구간에서
BCB 가 우세하면 "공유 prior 기반 무상태 헤더 압축"은 검토할 가치가 있다. 단 (1) 양측이 prior 를
배포·동기화해야 하고, (2) warm 반복 트래픽에선 동적 테이블에 밀린다. 위 표의 cold 열이 그 가능성의
실측 근거다 — 절대 우위가 아니라 **틈새(무상태·짧은 연결) 우위**로 읽어야 한다.
""".format(train_n=train_n, train_b=train_b, test_n=test_n, table=tbl(rows),
           bcb_summary=bcb_summary, bw=bw, cw=cw, ww=ww,
           cold_verdict=cold_verdict, warm_verdict=warm_verdict,
           resp_warm_note=resp_warm_note)
    with open(path, "w", encoding="utf-8") as f:
        f.write(body)


if __name__ == "__main__":
    sys.exit(main())
