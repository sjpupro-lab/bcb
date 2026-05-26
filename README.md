# BCB — Binary Compression by BT

**무손실 압축 시스템. 코드북 없음. BT(Bipedal Tree) prior + range coder.**
Lossless compression. Codebook-free. BT prior + range coder. All-integer path (MCU-capable, planned in v3).

Author: 호시 <jahyag@gmail.com> · Org: sjpupro-lab · License: MIT

---

## 한 줄 요약 / TL;DR

코드북(외부 chunk 사전) 없이, 양쪽(인코더·디코더)이 공유하는 학습된 BT prior + range coder
로 무손실 압축한다. 진짜 책 데이터에서 gzip/bz2/lzma 를 모두 능가한다 (학습 시 3.13×).

Without any codebook, BCB compresses losslessly using a learned BT prior shared by both
encoder and decoder, plus a range coder. On real book data it beats gzip/bz2/lzma
(3.13× when trained).

---

## 핵심 원리 / Core idea

### 약속된 공간 + 점 하나 = 압축 / A shared space + one point = compression

- **공간 (space)** — 인코더와 디코더가 함께 외워 둔, 학습된 BT prior (context → 다음 바이트 분포).
  전송 비용 0. 양쪽이 동일 코퍼스로 학습하면 같은 공간을 갖는다.
- **점 (point)** — 데이터 X 가 그 공간에서 차지하는 위치. range coder 의 정수 한 개로 표현된다.
- **전송 (transmit)** — 점의 좌표만 보낸다. 공간 자체는 보내지 않는다.

### 호시 통찰 / Hoshi's insight

코드북(외부 chunk 사전)은 BT 의 long-context 예측력을 가로막는다 → **제거**.
BT 가 "이 context 면 어떤 바이트열인지" 를 직접 학습하게 둔다.

A codebook (external chunk dictionary) blocks BT's long-context prediction power, so it is
removed. BT learns "given this context, what byte comes next" directly.

---

## 측정 결과 / Benchmarks

Pride and Prejudice 에서 4KB 발췌를 압축. BCB 는 발췌 직전의 코퍼스로 학습.
아래는 이 레포의 `make bench` 실측값 (offset 600000):

| 학습량 / train | BCB       | gzip-9 | bzip2-9 | xz-9  |
|----------------|-----------|--------|---------|-------|
| 0 KB           | 1.96×     | —      | —       | —     |
| 50 KB          | 2.78×     | 2.05×  | 2.19×   | 1.99× |
| 200 KB         | 3.04×     | —      | —       | —     |
| 500 KB         | **3.16×** | —      | —       | —     |

50 KB 학습만으로 bzip2-9 를 넘어서고, 500 KB 에서 세 압축기를 모두 능가한다.
학습된 데이터(반복 구조의 synthetic)에서는 200× 이상도 확인됨.
표준 압축기는 발췌 단독(공유 모델 없음)을 압축한 결과다.

`make bench` 로 본인 머신에서 재현 가능. (See `docs/benchmarks.md`.)

---

## 빌드 & 실행 / Build & run

```sh
make all          # build/bcb-cli, build/bcb-bench
make test         # v0 baseline 무손실 round-trip 검증
make bench        # gzip/bz2/lzma 와 비교 벤치마크
```

요구사항: C99 컴파일러 + libm (`-lm`). 외부 라이브러리 의존성 없음.

### CLI

```sh
build/bcb-cli encode input.txt out.bcb  -t tests/corpus/pride_and_prejudice.txt
build/bcb-cli decode out.bcb  restored.txt -t tests/corpus/pride_and_prejudice.txt
```

`-t` 로 준 학습 파일은 encode/decode 양쪽에서 반드시 동일해야 한다 (공유 prior).

---

## 레포 구조 / Layout

```
src/v0_baseline/      range coder + 24-byte context n-gram BT (현재 baseline)
src/v1_symmetric_dist/  대칭쌍 분포 활용 (작업 예정)
src/v2_hier_clock/      시계계층 좌표공간 (작업 예정)
src/v3_integer_bt/      정수 전용 BT, MCU 대응 (작업 예정)
src/v4_aux_channel/     SLIG/wave 보조채널 (작업 예정)
tests/                  round-trip + 벤치마크, corpus/ 에 Gutenberg 4권
tools/                  bcb-cli, bcb-bench
docs/                   theory.md, benchmarks.md, mcu.md
bindings/python/        ctypes wrapper (작업 예정)
```

---

## 정직한 한계 / Honest limits

- 랜덤 데이터는 압축 불가 (Shannon). BCB 도 못 한다. 정상.
- n-gram BT 의 한계 ≈ BPB 2.0 (약 4× 압축). 그 이상은 LLM/transformer 영역.
- 인코드 속도 ≈ 2 KB/s (바이트당 256 lookup). v3 에서 캐싱으로 가속 예정.
- full BT pool ≈ 256MB+. MCU 빌드는 v3 에서 별도 구성.

---

## License

MIT. See [LICENSE](LICENSE) and [AUTHORS](AUTHORS).
