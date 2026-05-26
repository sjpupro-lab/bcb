# BCB Benchmarks — 작은 메시지 / Small messages

BCB 의 실제 강점 구간: **작은 메시지(≤256B) + 공유 prior**. 여기서 BCB 를
brotli+dict, zstd+dict 와 정면 비교한다.

재현 / Reproduce:

```sh
make msgbench           # 사람이 읽는 표 (5개 시나리오): BCB vs brotli+dict vs zstd+dict
make msgbench-landmark  # 위 + BCB+lm(landmark prior) 열 추가
make msgbench-md        # 같은 측정을 markdown 표로
make msgbench-check     # baseline 대비 회귀 검사 (±2%)
```

> **landmark prior index**: `--landmark-k` 로 빈출 context 에 sharper cum 을 박으면 BCB 의 승리
> 구간이 넓어진다(HTTP ~256B, MQTT/RPC ~1KB). 압축비·속도 동시 향상, 무손실. 측정·한계는
> `docs/landmark.md` 참고.

> 4KB 영어 책 발췌(gzip/bzip2/xz) 기준의 옛 측정과 v0~v4 개발 기록은
> `docs/benchmarks_legacy.md` 로 옮겼다 (BCB 의 약한 구간, 참고용).

## 측정 방법 / Methodology

- 시나리오 generator(`tests/scenarios/*.py`)가 결정적(seed 고정) byte stream 을 만든다.
- 코퍼스 앞 **train-size(기본 50,000) 바이트 = 공유 prior**.
  - BCB: 이 구간으로 BT 학습.
  - brotli+dict: 같은 구간을 **raw shared dictionary** 로 attach (quality 11, lgwin 24).
  - zstd+dict: 같은 구간을 **raw content dictionary** 로 load (max level, frame overhead 최소화:
    contentSize/checksum/dictID 끔).
- 나머지 구간에서 `message_size` 바이트 윈도를 잘라 표본 메시지(기본 24개)로 압축, 평균.
- **측정 대상 = 각 코덱의 코어 출력 바이트** (외부 길이 framing 제외):
  BCB 는 range-coder payload (BCB1 컨테이너 12B 제외), brotli 는 스트림 출력(원본 길이 미저장),
  zstd 는 최소 frame.
- BCB 는 메시지마다 round-trip **무손실 검증**(아래 표 전부 `lossless: yes`).
- BCB 엔진은 **v3 정수 BT** (libm 없음, ~28× 빠른 hot path).

config: `train-size 50000`, `samples 24`, `message-sizes 64,128,256,512,1024,2048,4096`.
값은 코퍼스/표본에 따라 소폭 달라진다 (generator seed 고정으로 재현은 결정적).

## 측정 결과 / Results (이 레포 실측)

ratio = 원본/압축, 클수록 좋음. `winner` = 그 크기에서 가장 압축비 높은 코덱.

### HTTP/1.1 헤더 (`http_headers.py`)

| msg_size | BCB(x) | brotli+dict(x) | zstd+dict(x) | winner |
|---|---|---|---|---|
| 64   | **5.19** | 4.25  | 2.97  | **BCB** |
| 128  | 5.95 | **6.95**  | 5.18  | brotli |
| 256  | 5.96 | **7.98**  | 6.98  | brotli |
| 512  | 6.07 | **9.37**  | 8.90  | brotli |
| 1024 | 6.03 | **11.20** | 10.55 | brotli |
| 2048 | 5.99 | **12.92** | 12.29 | brotli |
| 4096 | 5.99 | **14.56** | 13.93 | brotli |

HTTP 텍스트 헤더는 LZ 친화적(반복 토큰 多) → brotli 가 128B부터 앞선다. BCB 는 64B 에서만 우위.

### IoT 센서 패킷 (`iot_packets.py`, 18B 바이너리)

| msg_size | BCB(x) | brotli+dict(x) | zstd+dict(x) | winner |
|---|---|---|---|---|
| 64   | **1.54** | 0.95 | 0.98 | **BCB** |
| 128  | **1.54** | 1.10 | 1.16 | **BCB** |
| 256  | **1.53** | 1.32 | 1.30 | **BCB** |
| 512  | 1.52 | **1.52** | 1.42 | brotli |
| 1024 | 1.48 | **1.73** | 1.52 | brotli |
| 2048 | 1.46 | **1.92** | 1.64 | brotli |
| 4096 | 1.47 | **2.04** | 1.72 | brotli |

64B 에서 brotli·zstd 는 **데이터를 오히려 키운다(0.95×, 0.98× < 1)** — 작은 바이너리에 프레임
overhead 가 크기 때문. BCB 는 1.54× 유지. **256B 까지 BCB 우위.**

### MQTT PUBLISH (`mqtt_messages.py`)

| msg_size | BCB(x) | brotli+dict(x) | zstd+dict(x) | winner |
|---|---|---|---|---|
| 64   | **4.03** | 2.24 | 2.10 | **BCB** |
| 128  | **4.22** | 2.80 | 2.80 | **BCB** |
| 256  | **4.31** | 3.62 | 3.36 | **BCB** |
| 512  | **4.39** | 4.31 | 4.11 | **BCB** |
| 1024 | 4.42 | **4.93** | 4.80 | brotli |
| 2048 | 4.44 | **5.42** | 5.29 | brotli |
| 4096 | 4.46 | **5.78** | 5.64 | brotli |

**512B 까지 BCB 우위** (5개 시나리오 중 가장 넓은 구간).

### syslog 로그 라인 (`log_lines.py`)

| msg_size | BCB(x) | brotli+dict(x) | zstd+dict(x) | winner |
|---|---|---|---|---|
| 64   | **3.18** | 1.95 | 1.72 | **BCB** |
| 128  | **3.33** | 2.42 | 2.27 | **BCB** |
| 256  | **3.38** | 3.08 | 2.85 | **BCB** |
| 512  | 3.38 | **3.52** | 3.35 | brotli |
| 1024 | 3.41 | **3.90** | 3.78 | brotli |
| 2048 | 3.42 | **4.20** | 4.07 | brotli |
| 4096 | 3.41 | **4.38** | 4.27 | brotli |

### gRPC-like RPC (`rpc_calls.py`, text path + binary body)

| msg_size | BCB(x) | brotli+dict(x) | zstd+dict(x) | winner |
|---|---|---|---|---|
| 64   | **3.49** | 1.91 | 2.08 | **BCB** |
| 128  | **3.70** | 2.40 | 2.83 | **BCB** |
| 256  | **3.81** | 3.04 | 3.45 | **BCB** |
| 512  | 3.82 | 3.62 | **3.92** | zstd |
| 1024 | 3.84 | 4.23 | **4.34** | zstd |
| 2048 | 3.88 | **4.74** | 4.67 | brotli |
| 4096 | 3.89 | **5.04** | 4.89 | brotli |

## 요약 / Summary

- **BCB 는 64B 에서 5개 시나리오 전부 1등.** 64B 에서 최선의 LZ 경쟁자 대비:
  HTTP +22%, IoT +57%, MQTT +80%, log +63%, RPC +68%.
- **256B 까지는 4/5 시나리오에서 BCB 우위** (HTTP 만 예외). MQTT 는 512B 까지.
- **교차점은 대략 512B~1KB.** 그 이상에서는 brotli/zstd 의 LZ77 long-range matching 이 이긴다.
- **메시지가 작을수록 BCB 우위가 커진다** — 핵심 통찰이 측정으로 확인됨.
- 모든 BCB 측정 무손실(round-trip 검증).

## 원 보고 수치와의 차이 / Prior-report numbers

재포지셔닝 기획서(파일 1)에 "검증된 사실"로 첨부됐던 표는 아래와 같다. 본 레포의 실측
(위 표)과 **방향은 일치**(작은 메시지에서 BCB 우위, 작을수록 우위↑)하나 **절대 압축비는 다르다**.
공정성을 위해 양쪽을 병기한다.

| 데이터 | 크기 | 원 보고 BCB | 원 보고 brotli+dict | 이 레포 BCB | 이 레포 brotli+dict |
|---|---|---|---|---|---|
| HTTP 헤더 | 64B  | 10.67× | 5.82× | 5.19× (64B) | 4.25× |
| HTTP 헤더 | 128B | 9.85×  | 8.00× | 5.95× | 6.95× |
| IoT 패킷  | 180B | 2.00×  | 1.55× | 1.53× (256B) | 1.32× |
| 영어 책   | 256B | 2.75×  | 2.31× | — (legacy 참고) | — |

차이의 원인(추정):
- **코퍼스가 다르다.** 원 보고가 어떤 HTTP/IoT 데이터를 썼는지 본 레포에 없어, 여기서는
  결정적 합성 generator 를 새로 만들었다 — 절대 압축비는 데이터 redundancy 에 크게 좌우된다.
- **brotli+dict 구성.** 본 레포는 quality 11 + 50KB raw shared dictionary 로 brotli 를 강하게
  세팅했다. 원 보고의 brotli 설정은 알 수 없다.
- **측정 단위.** 본 레포는 코어 코덱 출력만 센다(외부 길이 framing 제외).

**결론**: "작은 메시지에서 BCB 가 LZ+dict 를 이긴다"는 정성적 주장은 실측으로 **재현**된다
(64B 에서 +22~80%). 다만 원 보고의 절대 배수(10.67× 등)는 본 레포 합성 코퍼스에서는
재현되지 않으므로, README/문서는 **이 레포 실측값**을 1차 근거로 삼는다.

## 회귀 가드 / Regression guard

`tests/scenarios/baseline.json` 에 위 BCB 압축비를 고정해 두고, `make msgbench-check`
(CI 의 `.github/workflows/msgbench.yml`)가 매 PR 마다 ±2% 이탈을 검사한다. baseline 갱신:

```sh
python3 tests/scenarios/check_regression.py --bin build/bcb-msgbench --build build \
  --baseline tests/scenarios/baseline.json --update
```
