# BCB Benchmarks

재현: `make bench` (기본 코퍼스: `tests/corpus/pride_and_prejudice.txt`).
직접 지정: `build/bcb-bench <corpus.txt> [excerpt_offset] [excerpt_len]`.

## 측정 방법

- 코퍼스의 한 발췌(기본 4096 B)를 대상으로 압축비를 측정한다.
- BCB 는 발췌 **직전** 의 코퍼스 구간으로 학습한다 (학습량 sweep: 0 / 50 / 200 / 500 KB).
  인코더·디코더가 동일 코퍼스를 공유한다는 전제를 그대로 반영한다.
- 표준 압축기(gzip/bzip2/xz/zstd)는 발췌 단독(공유 모델 없음)을 압축한다.
- 모든 BCB 측정은 round-trip 무손실 검증(`lossless: yes`)을 동반한다.

## Pride and Prejudice — 4KB 발췌 (offset 600000)

이 레포의 `make bench` 실측값 (Gutenberg #1342, 772389 B):

| 학습량 / train | BCB       | lossless |
|----------------|-----------|----------|
| 0 KB           | 1.96×     | yes      |
| 50 KB          | 2.78×     | yes      |
| 200 KB         | 3.04×     | yes      |
| 500 KB         | **3.16×** | yes      |

| 표준 압축기 (발췌 단독) | ratio |
|--------------------------|-------|
| gzip-9                   | 2.05× |
| bzip2-9                  | 2.19× |
| xz-9                     | 1.99× |

BCB 는 50 KB 학습만으로 이미 bzip2-9 를 넘어서고, 500 KB 학습에서 3.16× 로
세 압축기를 모두 능가한다. 발췌 위치·코퍼스 판본에 따라 수치는 소폭 달라질 수 있다.

## v1 ablation — 단계 (a) 합=1 강제

`make v1-compare` 실측 (동일 학습/발췌, v0 vs v1a):

| 학습량 | v0      | v1a (합=1) | lossless |
|--------|---------|------------|----------|
| 0 KB   | 1.964×  | 1.966×     | yes      |
| 50 KB  | 2.785×  | 2.792×     | yes      |
| 200 KB | 3.036×  | 3.045×     | yes      |
| 500 KB | 3.156×  | **3.165×** | yes      |

**결론**: (a) 합=1 강제는 모든 학습량에서 v0 대비 작지만 일관된 이득(~0.3%)을 주고
무손실을 유지한다 → **채택**. v0 의 가변 total 양자화 대신 정확히 scale 로 재양자화하여
꼬리 빈에 낭비되던 확률 질량을 약간 회수한 결과다.

### 단계 (b)(c) 에 대한 측정 전 분석 — 호시에게 보고

(b) 대칭쌍 분할, (c) 시계계층 이진 분할은 **동일한 분포를 다른 방식으로 재부호화**하는 것이다.
산술부호의 사슬규칙에 의해 P(byte) = ∏(단계별 조건부 비트 확률) 이므로,
**같은 분포를 쓰는 한 코드 길이는 v0 와 정확히 동일**하다 (재양자화 오차만큼만 차이).
즉 (b)(c) 자체로는 압축 이득이 없다.

실질 이득은 BT 가 **단계(깊이)별로 서로 다른 통계를 학습**할 때(multi-resolution)만 생긴다.
v2(시계계층 다중좌표 context)로 이를 시도했으나 측정 결과 bucket fragmentation 으로 모두
악화되어 폐기됐다 (아래 "v2/v3/v4 탐색" 참고). 거시 통계는 context mix 가 아니라
v4 의 distribution blend 로 다룬다. v1 은 (a) 만 채택하여 마무리.

## v2/v3/v4 탐색 (2026-05-26, 4KB 발췌 / 50KB 학습 baseline)

여러 방향을 측정해 채택/폐기를 결정한 기록.

| 방향 | 결과 | 결정 |
|------|------|------|
| v2 시계계층 carry tick | baseline 대비 −1.5 ~ −180% | **폐기** |
| 학습 코퍼스 확장 (B) | 200KB 까지 +9.5%, 500KB+ 시간 폭발 | v3 자료구조 개선 후 재시도 |
| 보조채널: fnv-XOR mix (C) | 전부 악화 (−2 ~ −130%) | 메커니즘 거부 |
| 보조채널: distribution blend (C) | **+0.41% 일관 (4권 중 3권)** | **v4 의 진짜 형태** |

### v2 carry tick — 폐기

| 구성 | 압축비 | baseline 대비 |
|------|--------|---------------|
| baseline | 2.80× | – |
| B=[256,16,16,16] (pseudo-byte) | 2.76× | −1.5% |
| B=[16,16,16,16] | 2.36× | −18.6% |
| B=[256,16,16,16] (fnv-XOR mix) | 1.00× | −179.9% |

원인: carry tuple 에 따라 같은 24-byte context 가 다른 bucket 으로 흩어짐 → 학습 fragmentation.

### B 학습 코퍼스 확장 — 부분 효과 후 시간 폭발

| 학습량 | 압축비 | 학습 | 인코드 |
|--------|--------|------|--------|
| 50 KB | 2.42× | 0.4s | 2.9s |
| 200 KB | 2.65× | 5.1s | 13.4s |
| 500 KB | (60s timeout) | – | – |

원인: bloom 포화 + pool chain lookup O(n²). → v3 자료구조 최적화와 결합 필요.

### C distribution blend — 양수 발견 (v4 의 방향)

context 를 안 건드리고 byte type prior 를 별도 학습해 α=0.985 로 blend:

| 책 | baseline (B) | +aux (B) | 개선 |
|----|------|------|------|
| pride | 1464 | 1458 | +0.41% |
| frankenstein | 1586 | 1577 | +0.57% |
| alice | 690 | 691 | −0.14% |
| moby_dick | 1632 | 1624 | +0.49% |
| 합계 | 5372 | 5350 | **+0.41%** |

4권 중 3권 일관 개선 → 노이즈가 아닌 신호. type-prior 가 BT 미학습 거시 전이(알파벳→공백 등)를 보정.

### v4 byte_type — 레포 재현 (`make v4-aux`)

`src/v4_aux_channel/aux.c` 의 `AuxChannel` (byte_type, 7-type, α=0.985) 구현으로 재측정
(50KB 학습 / 4KB 발췌 @ off 60000, baseline=v1a):

| 책 | base(B) | +aux(B) | 개선 | lossless |
|----|---------|---------|------|----------|
| pride | 1518 | 1508 | +0.66% | yes |
| frankenstein | 1538 | 1528 | +0.65% | yes |
| alice | 1431 | 1421 | +0.70% | yes |
| moby_dick | 1634 | 1623 | +0.67% | yes |
| 합계 | 6121 | 6080 | **+0.67%** | yes |

**4권 전부** 일관 개선(+0.67%), 전부 무손실. 원 보고(+0.41%, 3/4)를 재현·상회.
(절대 바이트는 발췌/학습창 차이로 보고값과 다름 — 방향·일관성이 핵심.)

타입 분류: space / newline / lower / upper / digit / punct / other (7종).
blend: `P_final(b) = α·P_BT(b) + (1−α)·P(type|prev_type)·P(b|type)`, symdist 로 합=scale·각 빈≥1 보장.

### v4 4채널 ablation (`make v4-aux`)

4종 채널 단독 + 4채널 combo 측정 (α=0.985, baseline=v1a, 개선% = 더 작아진 비율):

| 책 | base(B) | byte_type | bigram | case | whitespace | combo(4) |
|----|---------|-----------|--------|------|------------|----------|
| pride | 1518 | +0.66% | +0.79% | +0.59% | +0.66% | +2.50% |
| frankenstein | 1538 | +0.65% | +0.72% | +0.65% | +0.72% | +2.67% |
| alice | 1431 | +0.70% | +0.77% | +0.56% | +0.77% | +2.66% |
| moby_dick | 1634 | +0.67% | +0.73% | +0.55% | +0.67% | +2.57% |
| **합계** | 6121 | +0.67% | +0.75% | +0.59% | +0.70% | **+2.60%** |

**모든 채널이 4권 전부에서 양수, 전부 무손실.** combo 는 +2.60% (예측 +1.5~3% 범위 상단).
단독 합(0.67+0.75+0.59+0.70=2.71) 대비 combo 2.60 → 채널 간 약한 중복, 거의 가산적.

채널 정의:
- **bigram_type**: `P(type | prev2, prev1)·P(b|type)` — 2차 타입 context.
- **case_pattern**: `P(case | prev_case)·P(b|case)`, case ∈ {lower, upper, non-letter}.
- **whitespace_phase**: `P(b | phase)`, phase = 직전 공백 이후 비공백 길이(0..12 clamp).
- combo: 각 채널 `adjust` 를 순차 체이닝 (per-channel α blend).

### 결정 요약

- **v2 (시계계층)**: 폐기.
- **v3 (정수 BT)**: 진행 + 자료구조 최적화(open addressing, bloom 16M, distribution caching) 확장.
- **v4 (보조채널)**: distribution blend 방식 `AuxChannel` 라이브러리. 4채널 **측정 완료** —
  단독 +0.6~0.8%, combo **+2.60%** (무손실). 다음: v3 정수화와 결합.

### 원 레퍼런스 (다른 발췌)

원 프로토타입 측정값(참고): 50KB 2.59× / 200KB 2.96× / 500KB 3.13×,
같은 발췌에서 gzip-9 2.12× · bz2 2.21× · lzma-9 2.02×.

## 코퍼스

`tests/corpus/` 에 Project Gutenberg 4권:

- `pride_and_prejudice.txt` (#1342)
- `frankenstein.txt` (#84)
- `alice_in_wonderland.txt` (#11)
- `moby_dick.txt` (#2701)

## 해석

학습량이 늘수록 BCB 의 학습된 prior P 가 데이터 분포에 가까워져 cross-entropy 가 낮아진다.
표준 압축기는 공유 모델이 없어 4KB 단독에서 사전 학습 이점을 얻지 못한다 —
이것이 BCB 의 설계 의도(공유 prior)가 드러나는 지점이다.
