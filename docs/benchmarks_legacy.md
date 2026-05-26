# BCB Benchmarks (Legacy — 4KB 텍스트 발췌)

> **참고용 (legacy).** 이 문서는 BCB 를 "범용 텍스트 압축기"로 포지셔닝하던 시기의
> 측정 기록이다 — 4KB 영어 책 발췌를 gzip/bzip2/xz 와 비교한다. 이는 LZ77 계열이
> 강한 큰 텍스트 영역으로, **BCB 의 약한 구간**임이 이후 측정으로 드러났다.
> BCB 의 실제 강점(작은 메시지 + 공유 prior)과 brotli+dict / zstd+dict 비교는
> **`docs/benchmarks.md`** 를 보라. 아래 v0~v4 개발/ablation 기록은 그대로 보존한다.
> This is the legacy "general-purpose text compressor" framing (4KB book excerpt
> vs gzip/bzip2/xz) — BCB's *weak* regime. See `docs/benchmarks.md` for the
> small-message benchmarks vs brotli+dict / zstd+dict.

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

## v3 단계 1 — distribution caching (`make v3-compare`)

`src/v3_integer_bt/btv3.c`: 활성 context 를 1회만 탐색하고 각 context 의 관측 next-byte 를
직접 순회(per-context 링크)하여 분포를 한 번에 계산. predict_byte 256회 호출 + 중복
context 탐색·exp/pow 제거. `pow(x,20)` 은 정수승(5회 곱)으로 대체.

v0 vs v3 (50KB 학습 / 4KB 발췌, 4권):

| 책 | v0(B) | v3(B) | ratioΔ | cum_relΔ | v0 인코드 | v3 인코드 | speedup | lossless |
|----|-------|-------|--------|----------|-----------|-----------|---------|----------|
| pride | 1522 | 1522 | +0.00% | 0.0000% | 4.09s | 0.11s | 36.2× | yes |
| frankenstein | 1542 | 1542 | +0.00% | 0.0000% | 4.55s | 0.12s | 36.6× | yes |
| alice | 1435 | 1435 | +0.00% | 0.0000% | 4.54s | 0.11s | 43.4× | yes |
| moby_dick | 1638 | 1638 | +0.00% | 0.0000% | 4.35s | 0.12s | 36.1× | yes |
| 합계 | 6137 | 6137 | +0.00% | – | 17.52s | 0.46s | **37.9×** | yes |

**v3 출력이 v0 와 비트단위 동일**(ratioΔ 0.00%, cum 폭 상대오차 0.0000%), 4권 전부 무손실,
인코드 평균 **37.9× 가속**. 정수승·재결합의 미세 오차가 alpha-blend·scale 양자화에 흡수돼
양자화된 cum 이 v0 와 완전히 일치했다.

## v3 단계 2 — open addressing + bloom 16M (`make v3-scale`)

chained hash(8M entries / 256K buckets → 평균 체인 32) 를 선형 탐사 open addressing 으로 교체:
BtEntry 슬롯 16M, CtxEntry 슬롯 8M (load ≤0.5), bloom 4M→16M bit. 같은 entry/freq 를 찾으므로
**분포는 v0 와 여전히 비트 동일** (`make v3-compare` PASS, ratioΔ 0.00%). context 탐색의 체인
워크도 사라져 인코드 가속이 37.9× → **74.2×** 로 추가 향상.

대규모 학습 스케일링 (moby_dick, 4KB 발췌):

| 학습량 | v0 학습 | v3 학습 | v3 인코드 | v3 압축비 | v3 entries | lossless |
|--------|---------|---------|-----------|-----------|------------|----------|
| 50 KB | 0.415s | 0.308s | 0.065s | 2.35× | 989,211 | yes |
| 200 KB | 7.169s | 1.450s | 0.078s | 2.66× | 3,760,109 | yes |
| 500 KB | —(skip) | 5.226s | 0.082s | 2.84× | 8,388,608 | yes |
| 1000 KB | —(skip) | 7.628s | 0.086s | 2.65× | 8,388,608 | yes |

v0 는 50K→200K 에서 0.42s→7.17s (4× 데이터에 ~17× 시간 = O(n²) 확인). v3 는 거의 선형
(200K 1.45s, 500K 5.2s, 1M 7.6s) 으로 **500K/1M 학습이 초 단위로 가능** (보고서의 "500K 시간 폭발" 해소).

**남은 병목**: `v3_entries` 가 500K 부터 BT_POOL 상한 8M 에 포화 → 더 큰 학습엔 pool 확대 필요
(1M 압축비가 500K 보다 낮은 것도 pool 포화 + 발췌 위치 차이 영향). pool 크기 조정은 후속 과제.

## v3 단계 3 — 정수 전용 hot path (log-domain)

`w = exp(n)·conf²⁰` 은 `conf²⁰` 가 uint64 를 초과(conf≤256 → 2¹⁶⁰)해 직접 정수화 불가.
**log2 영역**으로 우회: `log2_w = EXP_LOG2[n] + CONF_LOG2[p]` (둘 다 Q16 정수 LUT),
**byte별 자기 context max 로 정규화** → `w_int = EXP2(log2_w − max)` (∈(0,WSCALE], 정수).
비율 `Σw·p/Σw` 에서 정규화 상수가 상쇄돼 overflow 없이 분포를 얻는다. 분포 hot path 에 **double 없음**
(LUT 는 init 1회 생성; MCU 용 const 베이크는 단계 4).

> 설계 메모: 처음엔 *레벨별 전역 max* 로 정규화했더니, 지배 byte 보다 conf 가 4× 이상 낮은 byte 들이
> 전부 0 으로 underflow → 균등분포로 무너져 +17% 악화했다. **byte별 정규화**(scale-invariant)로
> 교정하니 −0.13% 로 들어왔다. 전역 carry-tick 을 *예측 context* 가 아니라 *정수 표현*에만 쓰라는
> 호시 지침과 같은 맥락 — 정규화 기준을 byte 단위로 잡는 것이 핵심.

v0 vs 정수 v3 (`make v3-compare`, 50KB, 4권):

| 책 | v0(B) | int-v3(B) | ratioΔ | lossless |
|----|-------|-----------|--------|----------|
| pride | 1522 | 1520 | −0.13% | yes |
| frankenstein | 1542 | 1540 | −0.13% | yes |
| alice | 1435 | 1433 | −0.14% | yes |
| moby_dick | 1638 | 1636 | −0.12% | yes |
| 합계 | 6137 | 6129 | **−0.13%** | yes |

±0.5% 이내(오히려 −0.13% 미세 개선), 4권 무손실, hot path 정수. 인코드는 v0 대비 **28.5×** 가속
(byte별 정규화로 단계2 의 74× 보다 작업량↑이나 double 제거가 목표). 대규모 학습도 정수 버전에서
1M 까지 무손실 유지(`make v3-scale`: 50K 2.36× / 200K 2.67× / 500K 2.84× / 1M 2.65×).

## v3 단계 4 — libm 제거 + aux 정수화 + MCU 빌드

- **libm 제거**: LUT(`EXP_LOG2`/`CONF_LOG2`/`EXP2_FRAC`) 를 정수 `log2`/`exp2`(integer sqrt 기반)
  로 init 에서 생성. `<math.h>` 불필요. v3-compare 결과 libm 버전과 **동일**(−0.13%) — 정수 LUT 정확.
- **aux 정수화** (`make v4-aux`): blend 를 Q16 α + 정수 prior 폭으로 재작성(`double` 제거),
  v3 정수 BT 위에서 측정. 4권, 50KB 학습, baseline=v3 단독:

| 채널 | 개선 |
|------|------|
| byte_type | +1.01% |
| bigram | +1.11% |
| case | +0.95% |
| whitespace | +1.08% |
| **combo(4)** | **+2.94%** |

전부 무손실. 정수 파이프라인(v3 BT + symdist + 정수 aux)에서 combo +2.94% — 기대 범위(+1.5~3%) 상단.

- **MCU 빌드** (`make meminfo`, `-DBCB_MCU`): footprint **546.5MB → 3.56MB**, 무손실 유지
  (압축비는 pool 포화로 desktop 4.55× → MCU 3.06×). 상세 `docs/mcu.md`.

## BT_POOL 확장 1 — 고정 크기 비교 (`make v3-pool`)

`-DBCB_POOL_BITS=N` 으로 BT_POOL=2^N 조정(기본 23=8M, 25=32M, 26=64M). MCU 빌드는 소형 유지.
diverse ~11.2MB 코퍼스(`tests/corpus/large.txt`, `fetch_large.sh` 로 생성)로 대규모 학습 측정:

| pool | footprint | 4MB 학습 | 10MB 학습 | 포화 |
|------|-----------|----------|-----------|------|
| 8M  | 546 MB  | 2.80× | 2.89× | 4MB·10MB 모두 포화 |
| 32M | 2.18 GB | 2.96× | 3.04× | 4MB·10MB 모두 포화 |
| 64M | 4.35 GB | **3.06×** | 3.04× | 4MB 미포화 / 10MB 포화 |

- **pool↑ → 압축비↑** (4MB 학습 2.80→2.96→3.06×). 4MB/10MB 학습 모두 가능·무손실.
- **diverse 텍스트 4MB 가 BT entry ~62M 개를 생성** → 8M/32M 는 즉시 포화, 64M 만 4MB 에서 미포화.
  10MB 학습은 64M 도 포화(필요 entry > 67M) → 고정 pool 의 한계.
- 고정 64M 사전할당은 4.35GB 로 비효율(미사용분 포함). entry 수에 비례하는
  **동적 할당**이 다음 단계(확장 2)의 동기.

## BT_POOL 확장 2 — 동적 할당 (기본 모드)

작게 시작(64K)해 `realloc` 로 2배씩 성장 + slot rehash(amortized O(1)). 상한 없음.
세 모드: **동적(기본)** / 고정(`-DBCB_POOL_BITS=N`) / MCU 소형(`-DBCB_MCU`). MCU·고정은 성장 안 함.

- **회귀**: 비포화 학습에선 동적 = 고정과 **비트 동일** (`make v3-compare` −0.13%, 4권 무손실).
  같은 entry/freq 를 찾으므로 slot 테이블 크기와 무관하게 분포가 같다.
- **메모리 비례화** (`make meminfo`, 30KB 학습):

| 빌드 | footprint(30KB 학습 후) | 압축비 | lossless |
|------|--------------------------|--------|----------|
| 고정 8M (이전) | 546 MB | 4.55× | yes |
| **동적** | **94.5 MB** | 4.55× | yes |
| MCU | 3.56 MB | 3.06× | yes |

30KB 학습은 entry ~62만 개 → 동적은 pool 을 36MB 까지만 키워 **546MB→94.5MB (5.8× 절감)**,
압축비·무손실 동일.

- **대규모 학습 — 포화 없음, 압축비 상승** (`make v3-pool`, 4KB 발췌):

| pool | 4MB 학습 | 10MB 학습 |
|------|----------|-----------|
| 고정 8M | 2.80× | 2.89× |
| 고정 32M | 2.96× | 3.04× |
| 고정 64M | 3.06× | 3.04× |
| **동적** | **3.10×** | **3.43×** |

10MB 학습에서 동적은 BT entry 144M 까지 성장(포화 X) → **3.43×**, 고정 64M(3.04×, 포화) 대비 큰 향상.
4MB 에서도 동적(3.10×) > 고정 64M(3.06×) — 고정 64M 은 BT pool(62M<67M) 은 안 찼지만
**CTX pool(32M) 이 포화**했기 때문. 동적은 BT·CTX 둘 다 키운다.
(peak_MB 는 pow2 반올림된 할당 용량이라 실제 RSS 보다 크다.)

### 결정 요약

- **v2 (시계계층)**: 폐기.
- **v3 (정수 BT)**: **단계 1~4 + pool 확장 1·2 완료**. distribution caching + open addressing +
  정수 hot path + libm 제거 + MCU. pool 은 동적(기본)/고정(`-DBCB_POOL_BITS`)/MCU 3모드.
  8M→64M 4MB 학습 2.80→3.06×; 동적은 메모리 비례(30KB 546→94.5MB) + 포화 없음(10MB 학습 **3.43×**). MCU 3.56MB.
- **v4 (보조채널)**: distribution blend `AuxChannel` 라이브러리, 정수화 완료. 정수 v3 파이프라인에서
  단독 +0.95~1.11%, **combo +2.94%** (무손실).

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
