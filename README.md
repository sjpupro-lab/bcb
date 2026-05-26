# BCB — Binary Compression by BT

**코드북 없는 무손실 압축. 학습된 BT prior + range coder. 전 구간 정수(MCU 대응).**
Codebook-free lossless compression: a learned BT (Bipedal Tree) prior + range coder,
with an all-integer, libm-free path that runs on microcontrollers.

Author: 호시 <jahyag@gmail.com> · Org: sjpupro-lab · License: MIT

---

## 한 줄 요약 / TL;DR

인코더와 디코더가 **같은 학습된 BT prior(공간)** 를 공유하고, 데이터를 그 공간 안의
**한 점(range coder 정수)** 으로 보낸다. 공간 자체는 전송하지 않으므로(양쪽이 외움)
실제 책 데이터에서 gzip·bzip2·xz 를 모두 능가한다.

Encoder and decoder share the same learned BT prior (the *space*); the data is sent as a
single *point* in that space (a range-coder integer). The space itself is never transmitted,
so on real text BCB beats gzip/bzip2/xz.

---

## 핵심 원리 / Core idea

**약속된 공간 + 점 하나 = 압축.**

- **공간 (space)** — 인코더·디코더가 함께 외워 둔 학습된 BT prior (context → 다음 바이트 분포).
  동일 코퍼스로 학습하면 양쪽이 같은 공간을 갖는다. 전송 비용 0.
- **점 (point)** — 데이터가 그 공간에서 차지하는 위치. range coder 의 정수 하나.
- **전송** — 점의 좌표만. 공간은 보내지 않는다.

**호시 통찰 / Hoshi's insight** — 코드북(외부 chunk 사전)은 BT 의 long-context 예측력을
가로막으므로 **제거**한다. BT 가 "이 context 면 다음 바이트는?" 을 직접 학습한다.
A codebook blocks BT's long-context prediction, so it is removed; BT learns the
next byte given a variable-length context directly.

---

## 측정 결과 / Benchmarks

Pride and Prejudice 4KB 발췌를 압축. BCB 는 발췌 직전 코퍼스로 학습 (공유 prior).
표준 압축기는 발췌 단독(공유 모델 없음) 압축. `make bench` 로 재현.

| 학습량 / train | BCB       | gzip-9 | bzip2-9 | xz-9  |
|----------------|-----------|--------|---------|-------|
| 0 KB           | 1.96×     | —      | —       | —     |
| 50 KB          | 2.78×     | 2.05×  | 2.19×   | 1.99× |
| 200 KB         | 3.04×     | —      | —       | —     |
| 500 KB         | **3.16×** | —      | —       | —     |

50 KB 학습만으로 bzip2-9 를 넘고, 500 KB 에서 세 압축기를 모두 능가. 대규모 학습(수 MB)에서는
더 큰 BT pool 로 압축비가 더 오른다 (아래 "메모리·pool" 참고). 상세: `docs/benchmarks.md`.

---

## 개발 단계 / Versions

| 단계 | 내용 | 결과 |
|------|------|------|
| **v0** `src/v0_baseline` | range coder + 24-byte context n-gram BT | baseline (500K 학습 3.16×) |
| **v1** `src/v1_symmetric_dist` | 분포 합=1 강제 재양자화 | 전 학습량 +0.3%, 무손실 |
| v2 (시계계층) | carry-tick 좌표를 BT context 에 mix | **폐기** (bucket fragmentation, −1.5~−180%) |
| **v3** `src/v3_integer_bt` | distribution caching → open addressing → 정수 hot path(log-domain) → libm 제거 + MCU | v0 대비 **−0.13%**, **~28× 가속**, 1M 학습 무손실, MCU **3.56MB** |
| **v4** `src/v4_aux_channel` | 거시 통계 보조채널 (distribution blend) | byte_type/bigram/case/whitespace, **combo +2.94%**, 무손실 |

설계·측정 기록은 `docs/theory.md`, `docs/benchmarks.md` 참고.

---

## 빌드 & 실행 / Build & run

요구사항: C99 컴파일러. v0 baseline 은 libm(`-lm`) 사용, **v3 정수 경로는 libm 없음**. 외부 라이브러리 의존성 없음.

```sh
make all          # bcb-cli, bcb-bench 빌드
make test         # 무손실 round-trip 검증
make bench        # gzip/bzip2/xz 비교
```

검증·측정 타깃:

```sh
make v1-compare   # v0 vs v1 (합=1) ablation
make v3-compare   # v0 vs v3 정수 BT — 동등성(±0.5%)·속도 (4권)
make v3-scale     # v3 대규모 학습 스케일링 (open addressing)
make v4-aux       # 보조채널 ablation (정수, v3 파이프라인, 4권)
make meminfo      # 메모리 footprint (desktop / MCU) + 무손실 점검
make v3-pool      # BT_POOL 고정 8M/32M/64M vs 동적 비교 (large.txt 필요)
```

대규모 학습 코퍼스: `sh tests/corpus/fetch_large.sh` → `tests/corpus/large.txt` (~11MB, git 미포함).

### CLI

```sh
build/bcb-cli encode input.txt out.bcb -t tests/corpus/pride_and_prejudice.txt
build/bcb-cli decode out.bcb restored.txt -t tests/corpus/pride_and_prejudice.txt
```

`-t` 학습 파일은 encode/decode 양쪽에서 동일해야 한다 (공유 prior).

---

## 메모리 · pool / Memory & pool

BT pool 크기는 압축비와 메모리를 가른다. 세 가지 빌드 모드:

| 모드 | 빌드 플래그 | pool | 용도 |
|------|------------|------|------|
| 동적 (기본) | (없음) | 작게 시작해 realloc 로 성장(상한 없음) | 메모리 비례, 대규모 학습 |
| 고정 | `-DBCB_POOL_BITS=N` | 2^N 고정 | 크기·압축비 비교 (8M/32M/64M) |
| MCU | `-DBCB_MCU` | 소형 고정(~3.56MB) | ESP32/RP2040 |

고정 pool 비교(4KB 발췌, diverse 코퍼스): 4MB 학습에서 8M→32M→64M 가 2.80→2.96→**3.06×**.
diverse 텍스트 4MB 가 BT entry 약 62M 개를 만들어 작은 pool 은 포화한다 → 동적 할당으로 해소.
상세: `docs/mcu.md`, `docs/benchmarks.md`.

---

## 레포 구조 / Layout

```
src/v0_baseline/        range coder + n-gram BT (reference)
src/v1_symmetric_dist/  분포 합=1 정규화
src/v3_integer_bt/      정수 BT (caching, open addressing, log-domain, MCU)
src/v4_aux_channel/     보조채널 (distribution blend)
tests/                  round-trip·벤치마크, corpus/ (Gutenberg)
tools/                  bcb-cli, bcb-bench, bcb-meminfo
docs/                   theory.md, benchmarks.md, mcu.md
```

---

## 정직한 한계 / Honest limits

- 랜덤 데이터는 압축 불가 (Shannon). 정상.
- n-gram BT 한계는 학습량·pool 에 좌우된다. 50K 학습 BPB≈2.0(4×)에서 대규모 학습으로 개선.
- v0 인코드 ≈ 2 KB/s → v3 에서 caching+정수화로 ~28× 가속.
- 대규모 학습은 메모리를 많이 쓴다 (diverse 10MB 학습 시 entry 수억 개). 동적 할당으로 메모리 비례화,
  MCU 빌드는 소형 pool 로 압축비를 메모리와 맞바꾼다.

---

## License

MIT. See [LICENSE](LICENSE) and [AUTHORS](AUTHORS). © 2026 호시 <jahyag@gmail.com>
