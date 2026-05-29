# BCB — Small-Message & Structured-Binary Lossless Compression

**공유 prior 를 가진 작은 메시지·고정 레코드 binary 를 위한 무손실 압축기.**
학습된 BT prior + range coder, 전 구간 정수(MCU 대응), 안정 C 라이브러리 API.
A lossless compressor for **small messages and fixed-layout binary records** that share a
learned prior. All-integer / libm-free core, embeddable stable C API.

Author: 호시 <jahyag@gmail.com> · Org: sjpupro-lab · License: MIT · API v0.2

---

## 한 줄 요약 / TL;DR

인코더와 디코더가 **같은 학습된 prior** 를 공유하고, 각 메시지를 그 prior 기준의 **한 점
(range coder 정수)** 으로 보낸다. prior 자체는 전송하지 않는다(양쪽이 보유). 작은 메시지·정형
binary 처럼 **공유 가능한 구조**가 있을 때 LZ(brotli/zstd)+dict 를 능가한다.

Encoder and decoder share a learned prior; each message is sent as a point in that space. The
prior is never transmitted. Where a *shared structure* exists (small messages, fixed binary
records), BCB beats brotli/zstd + dictionary.

---

## 언제 쓰나 / When to use

- ✅ **작은 메시지**(≤~512B): HTTP 헤더, MQTT/CoAP/gRPC, syslog, 푸시 알림, RPC. (`docs/benchmarks.md`)
- ✅ **고정 레코드 binary**: IoT 텔레메트리, Modbus, CAN 등. (`docs/structural.md`)
- ✅ 인코더·디코더가 **같은 prior 를 공유**할 수 있는 환경 (양쪽이 prior 파일 보유).
- ❌ **~1–2KB 이상**의 일반 데이터 → brotli/zstd 가 더 빠르고 더 잘 압축한다 (LZ77 long-range).
- ❌ prior 공유 불가·일회성·랜덤 데이터.

---

## 결과 / Results

### 작은 메시지 (text-like) — `make msgbench-landmark`

train 50K, samples 24, 코어 코덱 출력 바이트, round-trip 무손실. **BCB+lm** = landmark prior.

| 시나리오 | 크기 | BCB | **BCB+lm** | brotli+dict | zstd+dict | winner |
|---|---|---|---|---|---|---|
| MQTT | 64B | 4.03× | **4.60×** | 2.24× | 2.10× | BCB+lm |
| RPC | 64B | 3.49× | **4.00×** | 1.91× | 2.08× | BCB+lm |
| syslog | 64B | 3.18× | **3.44×** | 1.95× | 1.72× | BCB+lm |
| HTTP 헤더 | 256B | 5.96× | **8.52×** | 7.98× | 6.98× | BCB+lm |
| MQTT | 1024B | 4.42× | **5.16×** | 4.93× | 4.80× | BCB+lm |
| IoT 패킷 | 64B | **1.54×** | 1.54× | 0.95× | 0.98× | BCB |

작을수록 BCB 우위↑. landmark 가 LZ 를 넘는 상한을 넓힌다(HTTP ~256B, MQTT/RPC ~1KB). 그 이상은
brotli/zstd 가 이긴다(정직하게: `docs/benchmarks.md`, `docs/landmark.md`).

### 고정 레코드 binary (structural) — `make structural-bench`

position-aware schema(자리별 byte/delta 분포)로 압축. 실제 코딩 바이트, 전부 무손실:

| 시나리오 | base | **structural** | 이득 |
|---|---|---|---|
| binary_record (32B) | 1.39× | **5.35×** | +286% |
| per-device IoT (18B) | 1.28× | **3.99×** | +211% |
| Modbus (25B) | 1.44× | **3.67×** | +154% |
| CAN (16B) | 2.49× | **3.91×** | +57% |

gzip/zstd/brotli 는 이런 작은 binary 레코드에서 ~0.95×(오히려 키움). IoT 는 **per-device 스트림**에서
도약하고 다중 device interleave 공유 게이트웨이는 ~2× (`docs/structural_landmark.md`).

### HPACK (HTTP/2 헤더) — `docs/hpack_comparison.md`

cold-start 에서 BCB 5.87× vs HPACK 1.99× (≈3× 우위). warm 반복 request 는 HPACK 동적 테이블이 이김.

---

## 라이브러리 / Library API (`include/bcb.h`)

```sh
make build/libbcb.a
cc -Iinclude my_app.c build/libbcb.a -lm -o my_app
```

#### CMake (정적·동적 라이브러리, install, find_package) / cross-platform

Linux·macOS·Windows(MinGW) 공통. 정적(`libbcb.a`)·동적(`libbcb.so`/`.dll`/`.dylib`)
라이브러리를 모두 빌드하며, 동적 라이브러리는 **공개 API(`include/bcb.h`)만 export**한다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure        # 동적 라이브러리 round-trip 무손실 검증 포함
cmake --install build --prefix /usr/local
```

설치 후 다른 프로젝트에서:

```cmake
find_package(bcb REQUIRED)          # bcb::bcb (동적; 정적만 빌드 시 정적)
target_link_libraries(app PRIVATE bcb::bcb)
```
```sh
cc my_app.c $(pkg-config --cflags --libs bcb) -o my_app   # pkg-config 도 제공
```

옵션: `-DBCB_BUILD_SHARED=OFF` / `-DBCB_BUILD_STATIC=OFF` / `-DBCB_BUILD_TOOLS=OFF`
/ `-DBCB_BUILD_TESTS=OFF`. Windows(MinGW/PowerShell): `cmake -G "MinGW Makefiles" ...`.
정적 라이브러리를 Windows 에서 링크할 땐 소비자에 `BCB_STATIC` 가 자동 전파된다.

#### Python 바인딩 / Python bindings (`bindings/python/`)

cffi 로 C 코어를 직접 컴파일한 자체 완결 확장. 설치·사용·API 는 `bindings/python/README.md`.

```python
import bcb
with bcb.Prior("sensors.bcb-prior") as p:        # 컨텍스트 매니저
    comp = p.compress(b"...packet...")           # checksum/prior_id 토글
    back = p.decompress(comp, original_len=n)     # BcbError 예외 매핑
```

```c
#include "bcb.h"
BcbPrior *p = bcb_prior_open("sensors.bcb-prior");        /* mmap; 또는 _from_memory */
uint8_t out[/* >= */ 0]; size_t cap = bcb_compress_bound(msg_len);
/* ... out 을 cap 크기로 확보 ... */
ssize_t n = bcb_compress(p, msg, msg_len, out, cap);      /* 자기 기술적 컨테이너 */
ssize_t m = bcb_decompress(p, out, (size_t)n, back, msg_len);   /* m==msg_len, 무손실 */
bcb_prior_close(p);
```

- one-shot + `BcbEncoder`/`BcbDecoder` 핸들, `bcb_compress_bound`, `bcb_prior_id`,
  `bcb_strerror`/`bcb_version`, `BcbStatus` 에러 코드.
- **스레드 안전**: 핸들마다 인스턴스 상태; prior·LUT 읽기 전용 공유(같은 prior 동시 사용 가능).
  `make threads-test` 8스레드 동시 무손실.
- **CRC32 무결성**(기본 on) → 손상 시 `BCB_ERR_CORRUPTED`. `bcb_encoder_set_checksum` 으로 off.
- **prior id**(SHA-256 앞 16B): `bcb_encoder_set_prior_id` 로 압축본에 박으면 디코더가 prior
  불일치를 `BCB_ERR_PRIOR_ID_MISMATCH` 로 즉시 잡는다.

전체 레퍼런스·계약·한계: `docs/api.md`.

### CLI / prior 빌드

```sh
make all
build/bcb-prior-build train.txt out.bcb-prior --train-size 50000   # 기본 BT
build/bcb-prior-build train.txt out.bcb-prior --landmark-k 512      # + landmark (text-like)
build/bcb-prior-build train.bin out.bcb-prior --schema-record-size 18  # structural (binary)
build/bcb-cli encode msg.bin out.bcb --prior out.bcb-prior
build/bcb-cli decode out.bcb msg.out --prior out.bcb-prior
```

---

## 핵심 원리 / Core idea

**약속된 공간(prior) + 점 하나(range coder 정수) = 압축.** prior 는 context→다음 바이트 분포를
학습한 모델이고, 양쪽이 외워 두므로 전송 비용 0. 데이터는 그 공간에서의 좌표 하나로 전송된다.
코드북(외부 chunk 사전)은 long-context 예측을 가로막으므로 제거하고, BT 가 직접 학습한다.

**prior 의 세 가지 강화** (모두 정수 양자화 → 무손실):
- **landmark** — 빈출 context 에 sharper cum 을 박아 hit 시 예측을 건너뛴다(압축비·속도 동시↑).
- **structural** — 고정 레코드의 *자리별* 분포(byte/delta). 직전 레코드 기준 순환 시프트로 delta 도
  실제 byte 를 코딩하면서 효과를 얻는다.
- **mmap prior** — prior 를 파일로 공유해 재학습 없이 **즉시 시작**(300KB 학습 prior 3.74s→0.028s).

---

## 빌드 & 검증 / Build & test

```sh
make test               # round-trip 무손실 (v0 기준)
make msgbench           # 작은 메시지: BCB vs brotli+dict vs zstd+dict  (libbrotli-dev, libzstd-dev)
make msgbench-landmark  # + BCB+landmark 열
make structural-bench   # 고정 레코드 binary: base vs structural
make api-test           # 공개 API round-trip + 손상 검출 + prior-id
make threads-test       # 멀티스레드 동시 encode/decode 무손실
make prior              # mmap prior 동등성(in-memory=mmap) + RSS·처리량
```

CI(`.github/workflows/msgbench.yml`)가 매 PR 마다 회귀·무손실(msgbench-check, prior-equiv,
landmark-verify, structural-verify, api-test, threads-test)을 검사한다.

요구: C99. 코어는 libm 없음(측정 도구만 -lm). 작은 메시지 벤치는 `libbrotli-dev`, `libzstd-dev`.

---

## 개발 단계 / Versions

| 단계 | 내용 | 결과 |
|------|------|------|
| **v0** `src/v0_baseline` | range coder + n-gram BT (reference) | baseline |
| **v1** `src/v1_symmetric_dist` | 분포 합=1 정규화 | +0.3%, 무손실 |
| **v3** `src/v3_integer_bt` | 정수 hot path(log-domain) + open addressing + libm 제거 + MCU | v0 −0.13%, ~28×, MCU 3.56MB |
| **v4** `src/v4_aux_channel` | 거시 통계 보조채널 (distribution blend) | combo +2.94%, 무손실 |
| **v5** `src/v5_mmap_prior` | mmap prior + landmark + structural schema | HTTP +30~42%, binary +57~286%, 무손실 |
| **v6** `src/v6_public` `include/bcb.h` | 안정 공개 라이브러리, thread-safe, CRC32, prior-id | API v0.2, libbcb.a |

(v2 시계계층 carry-tick 은 측정 후 폐기 — `docs/benchmarks_legacy.md`.)

---

## 레포 구조 / Layout

```
include/bcb.h           공개 API v0.2 (stable)
src/v0_baseline/        range coder + n-gram BT (reference)
src/v1_symmetric_dist/  분포 합=1 정규화
src/v3_integer_bt/      정수 BT (caching, open addressing, log-domain, MCU; thread-safe reader)
src/v4_aux_channel/     보조채널 (distribution blend)
src/v5_mmap_prior/      prior 직렬화 + mmap + landmark + structural schema + per-instance codec
src/v6_public/          공개 라이브러리 구현 (bcb_api.c)
tests/scenarios/        합성 generator (HTTP/IoT/MQTT/log/RPC, http2, binary_record/modbus/canbus)
tests/                  round-trip·API·thread·corpus
tools/                  bcb-cli, bcb-prior-build, bcb-msgbench, bcb-blockbench, bcb-landmark,
                        bcb-structbench, bcb-meminfo, bcb_vs_hpack.py, landmark/structural_bench.py
docs/                   api, benchmarks, use_cases, landmark, structural, structural_landmark,
                        mmap_prior, hpack_comparison, mcu, theory, benchmarks_legacy
```

---

## 정직한 한계 / Honest limits

- **~1–2KB 이상은 brotli/zstd 가 이긴다.** BCB 는 작은 메시지·정형 binary 전용.
- prior 를 공유할 수 없으면 이점이 없다. 랜덤/이미 압축된 데이터는 압축 불가(Shannon).
- 절대 압축비는 코퍼스 redundancy·배치 방식에 크게 좌우된다(IoT: per-device 6×+ vs interleave 2×).
- structural 실측은 idealized entropy 추정보다 낮다(양자화·per-message framing). `docs/structural.md`.
- 메시지 무결성은 CRC32(기본 on)로 검출하나 적대적 위변조 방지는 아니다.
- 측정 수치는 합성 generator 기준 — 실제 데이터·HAR 로 재측정 가능(`tests/scenarios/http2_real.py`).

원 기획서 대비 재현되지 않은 수치(예: HTTP 64B 10.67×, IoT +55%)는 각 docs 에 실측과 병기했다.

---

## License

MIT. See [LICENSE](LICENSE) and [AUTHORS](AUTHORS). © 2026 호시 <jahyag@gmail.com>
