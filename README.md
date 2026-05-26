# BCB — Small-Message Lossless Compression

**공유 prior 를 가진 작은 메시지를 위한 무손실 압축기. 학습된 BT prior + range coder, 전 구간 정수(MCU 대응).**
A lossless compressor optimized for **small messages with a shared prior**.
Learned BT (Bipedal Tree) prior + range coder, all-integer / libm-free path that runs on microcontrollers.

Author: 호시 <jahyag@gmail.com> · Org: sjpupro-lab · License: MIT

---

## 한 줄 요약 / TL;DR

인코더와 디코더가 **같은 학습된 BT prior(공간)** 를 공유하고, 데이터를 그 공간 안의
**한 점(range coder 정수)** 으로 보낸다. 공간은 전송하지 않는다(양쪽이 외움).
**메시지 ≤256B** 구간(HTTP 헤더, IoT 패킷, MQTT, 로그, RPC)에서 BCB 는 64B 기준
brotli+dict 를 **+22~80%** 앞선다. **~2KB 이상이면 brotli/zstd 를 써라** — 더 빠르고 더 잘 압축한다.

Encoder and decoder share the same learned BT prior (the *space*); the data is sent as a single
*point* in that space. The space is never transmitted. For messages **≤256 bytes**, BCB beats
brotli+dict by **22–80% at 64B**; for data larger than ~2KB, use brotli or zstd instead.

---

## BCB 가 이기는 곳 / When BCB wins

`make msgbench-landmark` 실측 (train 50,000, samples 24, 코어 코덱 출력 바이트, round-trip 무손실).
ratio = 원본/압축, 클수록 좋음. **BCB** = 적응형 공유 prior(메시지별), **BCB+lm** = frozen 공유 prior
+ landmark index. 자세히는 `docs/benchmarks.md`, `docs/landmark.md`.

| 시나리오 | 크기 | BCB | **BCB+lm** | brotli+dict | zstd+dict | winner |
|---|---|---|---|---|---|---|
| MQTT     | 64B   | 4.03× | **4.60×** | 2.24× | 2.10× | BCB+lm |
| RPC      | 64B   | 3.49× | **4.00×** | 1.91× | 2.08× | BCB+lm |
| syslog   | 64B   | 3.18× | **3.44×** | 1.95× | 1.72× | BCB+lm |
| HTTP 헤더 | 256B  | 5.96× | **8.52×** | 7.98× | 6.98× | BCB+lm |
| MQTT     | 1024B | 4.42× | **5.16×** | 4.93× | 4.80× | BCB+lm |
| RPC      | 1024B | 3.84× | **4.52×** | 4.23× | 4.34× | BCB+lm |
| IoT 패킷 | 64B   | **1.54×** | 1.54× | 0.95× | 0.98× | BCB |

- **landmark 가 BCB 의 승리 구간을 넓힌다**: brotli/zstd 를 넘는 상한이 HTTP ~256B, MQTT/RPC ~1KB,
  syslog ~512B (base 만일 땐 각각 ~64B/~512B/~256B). HTTP 256B 에서 BCB+lm **8.52×** vs brotli 7.98×.
- IoT(고엔트로피 binary)는 landmark 미적중 → BCB+lm 이득 0, 적응형 base 가 더 낫다(아래 참고). 64B 에서
  brotli·zstd 는 프레임 overhead 로 **데이터를 키운다(<1.0×)**.
- **메시지가 작을수록 BCB 우위가 커진다.**

## BCB 가 지는 곳 / When BCB loses (정직하게)

| 시나리오 | 크기 | BCB | BCB+lm | brotli+dict | zstd+dict | winner |
|---|---|---|---|---|---|---|
| HTTP 헤더 | 512B  | 6.07× | 8.89× | **9.37×** | 8.90× | brotli |
| MQTT     | 4096B | 4.46× | 5.20× | **5.78×** | 5.64× | brotli |
| syslog   | 1024B | 3.41× | 3.74× | **3.90×** | 3.78× | brotli |
| IoT 패킷 | 4096B | **1.47×** | 1.33× | **2.04×** | 1.72× | brotli |
| HTTP 헤더 | 4096B | 5.99× | 8.60× | **14.56×** | 13.93× | brotli |

교차점은 landmark 로 올라가지만, 그 이상(대략 HTTP 512B / MQTT·RPC 2KB)에서는 LZ77 long-range
matching(brotli/zstd)이 이긴다. **큰 데이터엔 brotli/zstd 를 쓰라.**

> **BCB+lm 주의(정직)**: BCB+lm 은 frozen(stateless) 모드라, landmark 가 거의 안 맞는 데이터(IoT)에선
> 적응형 base 보다 약간 낮을 수 있다(예 IoT 4096B 1.33× < base 1.47×). landmark 이득은 반복적
> 텍스트형(HTTP/MQTT/log/RPC)에 집중된다. 원 기획서 수치(예 MQTT 64B +50~98%)는 재현되지 않으며
> 실측은 +8~42%(HTTP 최대) 다 — `docs/landmark.md` 에 병기.

## 타겟 사용처 / Target use cases

- IoT 텔레메트리 패킷 (10–500B) — `tests/scenarios/iot_packets.py`
- HTTP/2 헤더 압축 대안 (소형 헤더) — `tests/scenarios/http_headers.py`. **HPACK 비교**:
  cold-start 에서 BCB 5.87× vs HPACK 1.99× (≈3× 우위), warm 반복 request 는 HPACK 우세 — `docs/hpack_comparison.md`.
- MQTT / CoAP / gRPC 소형 메시지 — `tests/scenarios/mqtt_messages.py`, `rpc_calls.py`
- 임베디드 로그 라인, 푸시 알림, RPC 호출 — `tests/scenarios/log_lines.py`

자세한 적용·비적용 기준은 `docs/use_cases.md`.

## 속성 / Properties

- 전 구간 정수 파이프라인 (no float, no libm) — v3 정수 BT.
- MCU 빌드: **3.56MB** (ESP32 / RP2040 / STM32H7), `docs/mcu.md`.
- 데스크톱 동적 빌드: 학습 데이터에 비례해 pool 성장 (상한 없음).
- 여러 데이터 종류에서 round-trip 무손실 검증 (CI `make msgbench-check`).
- **인코더와 디코더는 같은 학습 prior 를 공유해야 한다.**
- **mmap prior** (`.bcb-prior`): prior 를 빌드해 파일로 공유 — 재학습 없이 즉시 시작
  (300KB 학습 prior 기준 3.74s→**0.028s**), 비트 동일·무손실, 프로세스 간 page 공유.
  단, 단일 프로세스 RSS 는 해시 조회 분산 탓에 <5MB 까지 줄지 않는다(sub-6MB 는 MCU 빌드).
  상세·정직한 한계: `docs/mmap_prior.md`.
- **landmark prior index** (`--landmark-k`): prior 안 빈출 context 에 sharper cum 을 박아
  hit 시 predict 를 건너뛴다 — 압축비·속도 동시 향상, 무손실(저장된 정수 cum 을 enc/dec 가 공유).
  실측: **HTTP 헤더 +30~42% & ~4× 속도**, MQTT/syslog +8~16%, **IoT 0%**(byte-context 미적중).
  message 단위 random access 가능(sub-message 는 불가 — 정직한 한계). 상세: `docs/landmark.md`.
- **structural (position-aware) landmark** (v6, 측정 단계): 고정 레코드 binary 데이터를 *위치별* 분포로
  압축. 측정(`make structbench`, entropy 추정): binary_record/modbus **+71~73%**, CAN +41%.
  **IoT 는 배치가 좌우 — per-device stream 에선 pos-delta 6.45× (5×+ 가능)**, 다중 device interleave
  공유 게이트웨이는 ~2.0×. 인코더 통합은 후속(PR-2). 상세·정직한 한계: `docs/structural_landmark.md`.

---

## 핵심 원리 / Core idea

**약속된 공간 + 점 하나 = 압축.**

- **공간 (space)** — 인코더·디코더가 함께 외워 둔 학습된 BT prior (context → 다음 바이트 분포).
  동일 코퍼스로 학습하면 양쪽이 같은 공간을 갖는다. 전송 비용 0.
- **점 (point)** — 데이터가 그 공간에서 차지하는 위치. range coder 의 정수 하나.
- **전송** — 점의 좌표만. 공간은 보내지 않는다.

코드북(외부 chunk 사전)은 BT 의 long-context 예측을 가로막으므로 제거한다. BT 가
"이 context 면 다음 바이트는?" 을 직접 학습한다. A codebook blocks BT's long-context
prediction, so it is removed; BT learns the next byte given a variable-length context directly.

---

## 빌드 & 실행 / Build & run

요구사항: C99 컴파일러. v3 정수 경로는 libm 없음. **작은 메시지 벤치는 `libbrotli-dev`, `libzstd-dev` 필요.**

```sh
make all          # bcb-cli, bcb-bench
make test         # 무손실 round-trip 검증

# 작은 메시지 벤치 (BCB vs brotli+dict vs zstd+dict)
make msgbench         # 5개 시나리오 표
make msgbench-md      # markdown 표 (docs 갱신용)
make msgbench-check   # baseline 대비 회귀 검사 (±2%)
```

CLI (공유 prior `-t` 는 encode/decode 양쪽 동일해야 함):

```sh
build/bcb-cli encode in.txt out.bcb -t tests/corpus/pride_and_prejudice.txt
build/bcb-cli decode out.bcb restored.txt -t tests/corpus/pride_and_prejudice.txt
```

mmap prior — prior 를 한 번 빌드해 파일로 공유 (재학습 없이 즉시 시작):

```sh
build/bcb-prior-build train.txt prior.bcb-prior --train-size 50000
build/bcb-cli encode msg.bin out.bcb --prior prior.bcb-prior
build/bcb-cli decode out.bcb msg.out --prior prior.bcb-prior
make prior            # 동등성(in-memory=mmap 비트 동일) + RSS·처리량 측정
```

레거시 텍스트 벤치(gzip/bzip2/xz vs 4KB 책 발췌)·v0~v4 개발 기록: `make bench`, `docs/benchmarks_legacy.md`.

---

## 개발 단계 / Versions

| 단계 | 내용 | 결과 |
|------|------|------|
| **v0** `src/v0_baseline` | range coder + 24-byte context n-gram BT | reference |
| **v1** `src/v1_symmetric_dist` | 분포 합=1 강제 재양자화 | +0.3%, 무손실 |
| v2 (시계계층) | carry-tick 좌표를 BT context 에 mix | **폐기** (fragmentation) |
| **v3** `src/v3_integer_bt` | caching → open addressing → 정수 hot path → libm 제거 + MCU | v0 대비 −0.13%, ~28× 가속, MCU 3.56MB |
| **v4** `src/v4_aux_channel` | 거시 통계 보조채널 (distribution blend) | combo +2.94%, 무손실 |
| **v5** `src/v5_mmap_prior` | prior 직렬화 + mmap 로드 (frozen) + landmark prior index | 비트 동일·무손실, 즉시 시작, HTTP +30~42% & ~4× |

설계·측정 기록: `docs/theory.md`, `docs/benchmarks.md`, `docs/benchmarks_legacy.md`, `docs/mcu.md`, `docs/mmap_prior.md`.

---

## 레포 구조 / Layout

```
src/v0_baseline/        range coder + n-gram BT (reference)
src/v1_symmetric_dist/  분포 합=1 정규화
src/v3_integer_bt/      정수 BT (caching, open addressing, log-domain, MCU)
src/v4_aux_channel/     보조채널 (distribution blend)
src/v5_mmap_prior/      prior 직렬화 + mmap 로드 (frozen)
tests/scenarios/        작은 메시지 generator (HTTP/IoT/MQTT/log/RPC, http2, binary_record/modbus/canbus) + 회귀 baseline
tests/corpus/           Gutenberg 4권 (레거시 텍스트 벤치)
tools/                  bcb-cli, bcb-bench, bcb-msgbench, bcb-meminfo, bcb-prior-build, bcb-prior-test, bcb-blockbench, bcb-landmark, bcb_vs_hpack.py, landmark_bench.py
docs/                   benchmarks(작은 메시지), use_cases, theory, mcu, benchmarks_legacy, mmap_prior, hpack_comparison, landmark, structural_landmark
```

---

## 정직한 한계 / Honest limits

- **≳1–2KB 데이터는 brotli/zstd 가 이긴다.** BCB 는 작은 메시지 전용.
- prior 를 공유할 수 없으면 이점이 없다. 일회성·임의 데이터에 부적합.
- 랜덤/이미 압축된 데이터는 압축 불가 (Shannon). 정상.
- 절대 압축비는 코퍼스 redundancy 에 크게 좌우된다 — 본 레포 수치는 합성 generator 기준.

---

## License

MIT. See [LICENSE](LICENSE) and [AUTHORS](AUTHORS). © 2026 호시 <jahyag@gmail.com>
