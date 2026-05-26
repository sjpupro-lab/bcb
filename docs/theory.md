# BCB Theory — 호시 통찰과 수학적 등가물

이 문서는 호시의 직관적 표현을 표준 정보이론/산술부호화 용어에 매핑한다.

## 1. 약속된 공간 + 점 = 압축

| 호시 표현 | 수학적 등가물 |
|-----------|---------------|
| 약속된 공간 | 인코더·디코더가 공유하는 확률모델 P (학습된 BT prior) |
| 점 하나 | 데이터 X 의 arithmetic code, 즉 [0,1) 구간의 한 실수 (정수로 표현) |
| 점의 좌표만 전송 | code 길이 ≈ −Σ log₂ P(xᵢ | context) 비트 |
| 공간은 전송 안 함 | 모델 P 는 양쪽이 동일 코퍼스로 학습 → 채널 비용 0 |

압축률은 결국 cross-entropy H(X; P) 로 결정된다. P 가 데이터의 실제 분포에 가까울수록
(= 학습이 잘 될수록) code 가 짧아진다. 이것이 학습량 sweep 에서 압축비가 오르는 이유다.

## 2. 코드북 제거 — 호시 핵심 통찰

코드북은 "자주 나오는 chunk → 짧은 심볼" 매핑이다. 이는 고정 길이 토큰 경계를 강제하여
BT 의 가변 길이 long-context 예측을 끊는다. BCB 는 코드북을 두지 않고, BT 가
context(최대 24바이트) → 다음 바이트 분포를 직접 학습하게 한다.

수학적으로: 코드북은 P 를 chunk 단위 marginal 로 근사하지만, BCB 는
P(byte | 가변길이 context) 를 직접 모델링한다 (PPM/CM 계열과 동족).

## 3. 분포의 대칭쌍 — v1 작업 항목의 근거

BT 가 내는 256-byte 분포는 Σ P(b) = 1 인 제약을 가진다.

| 호시 표현 | 수학 |
|-----------|------|
| 합이 1인 대칭쌍 | Σ P(b) = 1 → P(b₂₅₅) = 1 − Σ P(b₀..₂₅₄) |
| 절반이 어딘지 자동 | 분포 mode 기준 좌/우 분할이 context 로 결정적 |
| 정해진 공간 재귀 | 256 → 128 → 64 → … 이진분할, 양쪽 동일 순서 |

(a) 합=1 강제는 정확히 scale 로 재양자화하여 꼬리 빈의 확률 낭비를 줄인다.
측정 결과 전 학습량에서 +0.3% 일관 이득 + 무손실 → **채택** (`docs/benchmarks.md`).
(b)(c) 대칭/계층 분할은 동일 분포 재부호화라 자체 이득이 없음(사슬규칙) → 단독 구현 보류.

## 4. 시계계층 좌표 (v2) — 측정 후 폐기

v2 는 (계층/위치/방향/각도) carry tick 좌표를 BT hash 에 mix 하려 했다.
측정 결과 baseline 대비 −1.5 ~ −180% 로 모두 악화. 원인은 **bucket fragmentation**:
같은 24-byte context 가 carry tuple 에 따라 다른 bucket 으로 흩어져 학습이 조각난다.
50KB 학습으로는 carry 좌표공간을 cover 하기에 턱없이 부족했다.

→ **v2 작업 항목 폐기.** 거시 통계는 context mix 가 아니라 v4 의 distribution blend 로
다룬다 (아래 §6). 자세한 수치는 `docs/benchmarks.md` 참고.

## 5. 정수 전용 + 자료구조 최적화 (v3)

현재 `predict_byte` 는 `exp`, `pow` 등 double 연산을 쓴다. v3 는 이를 정수 LUT 로
대체하여 MCU(ESP32/RP2040)에서도 동작하게 한다. range coder 는 이미 32-bit 정수만 쓴다.
추가로 §2(대규모 학습)에서 드러난 O(n²) 병목을 풀기 위해 다음을 포함한다:
chain → open addressing, bloom 4M→16M bit 확장, distribution caching(byte당 256 lookup 제거).

## 6. 보조채널 — distribution blend (v4)

context 를 건드리지 않고, 별도로 학습한 **거시 통계 prior** 를 BT 분포와 가중 blend 한다:

```
prior(b)   = P(type(b) | prev_type) × P(b | type(b))
P_final(b) = α · P_BT(b) + (1−α) · prior(b)
```

context fragmentation 없이 BT 가 못 잡는 거시 전이(예: 알파벳→공백)를 보정하는 것이 핵심.
v4 는 이 blend 방식의 `AuxChannel` 라이브러리로 구현하며, 4종 채널을 제공한다:
byte_type / bigram_type / case_pattern / whitespace_phase. 각 채널의 `adjust` 를 순차
체이닝하여 결합한다.

측정(α=0.985, 50KB 학습/4KB 발췌, 4권 전부 무손실): 단독 +0.6~0.8%,
**4채널 combo +2.60%** (`docs/benchmarks.md`). 채널이 거의 가산적으로 누적된다.
