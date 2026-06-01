# BCB — 제품 사양표 / Spec Sheet

> 모든 값은 (a) 소스 코드, (b) 빌드 산출물, (c) 실측 명령 출력 중 하나를 근거로 한다.
> 측정 환경(달리 명시 없으면): **gcc 13.3.0, x86-64, Ubuntu 24.04**. 근거 명령은 각 행에 표기.
> 확인 불가/미측정 항목은 빈칸 대신 **"미측정"** 또는 **"미지원"** 으로 표기한다.

신뢰도 등급(표의 `[등급]` 열):
- **CI** — CI 워크플로에서 자동 빌드/테스트로 검증됨
- **SRC** — 소스 코드에서 직접 확인됨
- **MEAS** — 이 머신에서 실제 명령을 돌려 측정함
- **UNV** — 미검증·미측정 (설계상 의도이거나 추정)

---

## 1. 지원 플랫폼 / 아키텍처

| 항목 | 값 | 근거 | [등급] |
|------|----|----|--------|
| Linux x86-64 | 빌드 + 테스트 (round-trip·API·thread·msgbench 무손실) | `.github/workflows/msgbench.yml` (ubuntu-latest, `make test/api-test/threads-test/...`) | CI |
| Windows x86-64 (MSVC) | 빌드 + 테스트 (DLL round-trip via ctest) | `.github/workflows/windows.yml` (windows-latest, CMake/MSVC + ctest) | CI |
| macOS (universal) | 빌드만 (release 번들), 테스트는 sanity ctest | `.github/workflows/release.yml` build-native matrix (macos-latest) | CI |
| ARM64 / aarch64 (Linux) | **네이티브 CI 빌드·테스트 없음.** Python wheel만 cibuildwheel로 빌드 설정(QEMU 미설정 — 실제 산출 미확인) | `bindings/python/pyproject.toml` `[tool.cibuildwheel.linux] archs=["x86_64","aarch64"]`; release.yml에 QEMU 스텝 없음 | UNV |
| ARM64 (macOS arm64) | wheel 빌드 설정만 존재 | pyproject `[tool.cibuildwheel.macos] archs=["x86_64","arm64"]` | UNV |
| ARM32 / armv7 | **미지원** (CI·설정 어디에도 없음) | 워크플로/설정 grep 결과 없음 | — |
| MCU 빌드 (`-DBCB_MCU`) | 소스 컴파일·footprint·무손실 round-trip 확인 (호스트에서 MCU 설정으로) | `make meminfo` → `bcb-meminfo-mcu` round-trip lossless | MEAS |
| 실제 MCU 칩 온디바이스 | **온디바이스 미측정.** ESP32(PSRAM)/RP2040을 설계 타깃으로 잡았으나 실칩 flash/RAM/속도 측정 없음 | `docs/mcu.md` (bare-metal 포팅 TODO 명시) | UNV |

> 정리: **CI에서 빌드·테스트 검증된 것 = Linux x86-64, Windows x86-64.** macOS는 CI 빌드+sanity 테스트.
> ARM·MCU 실칩은 **설계 타깃이나 미검증**.

---

## 2. 런타임 메모리 풋프린트

런타임(encode/decode)과 모델(prior)·빌드시점을 분리한다.

| 항목 | 값 | 근거 | [등급] |
|------|----|----|--------|
| Encoder/Decoder 핸들 인스턴스 상태 | `sizeof(BcbCodec)` = **2144 B** (+ 핸들 래퍼 16 B). window 24 B + read-only prior 포인터 + schema 작업버퍼 `sc_prev/sc_cur` (각 SCHEMA_MAX=1024 B) | `src/v5_mmap_prior/bcb_prior.c:566` 구조체 + `sizeof` 측정 | SRC+MEAS |
| Encoder/Decoder — 핫패스 힙 | **있음.** `cec_enc_finish`가 출력 버퍼를 `realloc`로 성장, `cec_decompress`가 `malloc(orig_len)`. 즉 메시지당 출력 크기만큼 힙 할당 발생 | `src/v0_baseline/ce_compress.c:38,188` | SRC |
| Encoder/Decoder — 쓰기 RAM | 인스턴스 상태(2144 B)는 per-call 쓰기. prior 테이블은 읽기 전용 | `bcb_prior.c` (rd = BtV3Reader, 가변 window만) | SRC |
| prior 모델 — MCU 고정 빌드 | **3.56 MB** (3,735,552 B), **고정**(코퍼스 무관, 성장 없음) | `make meminfo` → `bcb-meminfo-mcu` `TOTAL 3735552 B`; `-DBCB_MCU` 상수와 일치 | MEAS |
| prior 모델 — 데스크톱 동적 빌드 | **가변**(학습량 비례, `realloc` 성장). 30KB 코퍼스→**94.5 MB**, 8M-pool 운영점→546.5 MB | `make meminfo` → `bcb-meminfo` `TOTAL 99090432 B (94.50 MB)` | MEAS |
| prior — 읽기전용 배치 가능? | **예.** prior는 frozen 읽기전용; mmap(파일 page 공유) 또는 `bcb_prior_from_memory`(복사·소유)로 로드. flash/PSRAM 상주 가능 | `include/bcb.h:88-90`; `bt_v3_attach` frozen; CI `prior-equiv` | SRC+CI |
| 프로세스 VmHWM (in-memory, 50KB train, 128B msg) | **19.38 MB** | `make prior-rss` → `rss-mem VmHWM=19.38 MB` | MEAS |
| 프로세스 VmHWM (mmap) | **17.36 MB** | `make prior-rss` → `rss-mmap VmHWM=17.36 MB` | MEAS |
| 프로세스 VmHWM (MCU 빌드, in-memory) | **5.90 MB** | `make prior-rss` → `rss-mem(mcu) VmHWM=5.90 MB` | MEAS |
| 스택 사용량 | **미측정** (재귀 없음·고정 로컬 버퍼 사용이나 정량 측정 안 함) | — | 미측정 |

> 주의: 위 "3.56 MB / 94.5 MB"는 **학습된 prior 모델 테이블이 차지하는 RAM**이다. 코드(.text)·스택·
> 학습 임시메모리가 아니다. 데스크톱 값은 동적이라 고정 숫자가 아님.

---

## 3. 라이브러리 크기

| 항목 | 값 | 근거 | [등급] |
|------|----|----|--------|
| static `libbcb.a` (CMake Release, x86-64) | **67,442 B** (≈ 65.9 KB) | `ls -l /tmp/cmbuild/libbcb.a` | MEAS |
| static `libbcb.a` (Makefile build) | **63,994 B** | `ls -l build/libbcb.a` | MEAS |
| shared `libbcb.so.0.2.0` (x86-64) | **54,536 B** (≈ 53.3 KB) | `ls -l /tmp/cmbuild/libbcb.so.0.2.0` | MEAS |
| shared `.dll` (Windows) / `.dylib` (macOS) | **이 머신 미측정** (CI에서 빌드되나 바이트 크기 미기록) | release.yml에서 빌드됨, 크기 측정 안 함 | 미측정 |
| 코어 코덱 `.text` 합 (MCU 빌드, x86-64 codegen) | **27,014 B** (ce_compress 2513 + btv3 8023 + bcb_prior 13301 + bcb_api 3177) | `size -DBCB_MCU` 컴파일 객체들 | MEAS |
| 타깃 MCU(ARM Thumb) `.text` | **미측정** (위 27 KB는 x86-64 codegen이라 ARM 실측 아님) | — | 미측정 |

> 참고: `bcb_prior.o`는 약 4.0 MB BSS(`bf`/`df` = SCHEMA_MAX×256×8×2)가 있으나 이는
> **prior 빌드(`bcb_prior_save_with_schema`) 전용 static 버퍼**이며 라이브러리 파일 크기엔 포함 안 됨(BSS).
> `bt_model.o`(v0 reference)는 더 큰 BSS를 갖지만 **공개 런타임 경로 아님**(아래 §4).

---

## 4. 의존성

| 항목 | 값 | 근거 | [등급] |
|------|----|----|--------|
| 외부 서드파티 라이브러리 (코어) | **0개.** libc만. brotli/zstd는 **벤치 도구 전용**(`bcb-msgbench`), 코어/라이브러리 아님 | `bcb_api.c`/`btv3.c`/`bcb_prior.c` include grep | SRC |
| v3 정수 hot path (`btv3.c`) — libm | **불필요.** 객체에 exp/pow/log2 undefined 심볼 없음 | `nm /tmp/mcuobj/btv3.o` → libm 심볼 none | MEAS |
| 런타임 encode/decode 경로 | `bt_v3_distribution_r` (정수 전용) 사용 — libm 미사용 | `bcb_prior.c:602` `codec_dist`→`bt_v3_distribution_r` | SRC |
| **그러나 `libbcb.a`/`.so` 전체는 libm 링크함** | **예 — 정직히 표기.** `.so`가 `libm.so.6` 의존(`exp`,`pow`,`log2`). 출처: ① `bt_model.c`(v0 reference, `exp`/`pow`) ② `bcb_prior.c`의 **prior-빌드** 함수 `bcb_prior_save_with_schema`(`log2`) | `ldd libbcb.so.0.2.0` → libm.so.6; `nm` → bt_model.o(exp,pow), bcb_prior.o(log2) | MEAS |
| → 공개 런타임 압축/해제만 쓰면 libm 호출 도달하는가? | v0 `bt_v4`(exp/pow)는 **CLI·테스트 전용**, 공개 API 경로 아님. `log2`는 **prior 빌드시에만**. 즉 디바이스 **인코드/디코드 hot path는 libm-free**지만, **라이브러리를 통째로 링크하면 libm이 딸려온다** | 위 + `grep bt_v4` 호출부(ce_compress/symdist/cli/tests만) | SRC |
| 표준 헤더 — 코어 | `<stdlib.h>`, `<string.h>`, `<stdint.h>`. **`<stdio.h>` 없음**(btv3.c) | `btv3.c:18-20`, `bcb_api.c:17-18` | SRC |
| 표준 헤더 — `bcb_prior.c` | 위 + `<stdio.h>`(파일 I/O), `<math.h>`(빌드시 log2), POSIX `<sys/mman.h>` 등(또는 Windows `<windows.h>`) | `bcb_prior.c:16-25` | SRC |
| 표준 헤더 — 측정/도구 | `bcb-msgbench`는 `<brotli/*>`, `<zstd.h>` 추가 의존 (코어 아님) | `tools/bcb-msgbench.c:33-35` | SRC |
| 핫패스 malloc/free (모델 분포 계산) | **없음.** `malloc`/`realloc`/`calloc`은 학습·init·prior 로드/검증 경로에만. 분포 계산(`bt_v3_distribution_r`,`ctx_find_rd`)은 무할당 | `nm`+`grep malloc btv3.c` (학습/init/grow에만) | SRC |
| 핫패스 malloc/free (메시지 입출력 버퍼) | **있음.** 위 §2대로 `cec_enc_finish`/`cec_decompress`가 출력 버퍼를 힙 할당 | `ce_compress.c:38,188` | SRC |

> 한 줄 요약: **분포 모델은 정수·무할당·libm-free**지만, **라이브러리 전체는 (v0 참조코덱 + prior
> 빌드 경로 때문에) libm을 링크하고, 메시지 입출력 버퍼는 힙을 쓴다.** "코어가 libm-free"는
> v3 정수 분포 경로에 한정된 참이며, 바이너리 전체에 대한 주장이 아니다.

---

## 5. API / 언어 바인딩

| 항목 | 값 | 근거 | [등급] |
|------|----|----|--------|
| C 공개 API 버전 | **0.2.0** (`BCB_VERSION_MAJOR.MINOR.PATCH` = 0.2.0, `bcb_version()`→`"0.2.0"`) | `include/bcb.h:63-65,132` | SRC |
| C API 표면 | `bcb_prior_open`/`_from_memory`/`_close`, one-shot `bcb_compress`/`_decompress`, `BcbEncoder`/`BcbDecoder` 핸들, `bcb_compress_bound`, `bcb_prior_id`, `bcb_strerror`, `bcb_version` | `include/bcb.h` | SRC |
| Python 바인딩 방식 | **cffi**로 C 코어를 직접 컴파일한 자체완결 확장 (별도 .so dlopen 없음) | `bindings/python/pyproject.toml` (`cffi>=1.15`), `bcb_build.py` | SRC |
| wheel 빌드 시스템 | setuptools + cibuildwheel | pyproject `build-backend=setuptools.build_meta`, `[tool.cibuildwheel]` | SRC |
| wheel 대상 — Python | cp38–cp313 | pyproject `build="cp38-* ... cp313-*"` | SRC |
| wheel 대상 — 플랫폼 (CI 빌드) | Linux/macOS/Windows = release.yml `build-wheels` 매트릭스 (ubuntu/macos/windows-latest) | `.github/workflows/release.yml` | CI |
| wheel — aarch64/arm64 | **설정만 존재, 실제 산출 미확인** (QEMU 스텝 없음) | §1과 동일 | UNV |
| wheel 스모크 테스트 | `python -c "import bcb; print(bcb.version())"` (네이티브 확장 로드 확인) | pyproject `test-command` | SRC |
| PyPI 업로드 | 태그 + 수동승인 environment(`pypi`, OIDC) 뒤에서만; 자동 업로드 아님 | release.yml `publish-pypi` | SRC |

---

## 6. 디코드 / 인코드 속도

| 항목 | 값 | 근거 | [등급] |
|------|----|----|--------|
| 처리량 (in-memory, http_headers 50KB train, 128B 메시지) | **185 msgs/s** | `make prior-rss` → `rss-mem msgs/s=185` | MEAS |
| 처리량 (mmap) | **190 msgs/s** | `make prior-rss` → `rss-mmap msgs/s=190` | MEAS |
| 처리량 (MCU 빌드, in-memory) | **255 msgs/s** | `make prior-rss` → `rss-mem(mcu) msgs/s=255` | MEAS |
| encode/decode 분리 측정 | **미측정** (위 msgs/s는 round-trip 묶음) | — | 미측정 |
| MB/s 단위 | **미측정** (메시지/s만 측정) | — | 미측정 |
| 타깃 MCU 칩 속도 | **미측정** | — | 미측정 |

> 위 msgs/s는 **이 머신(x86-64, gcc 13.3.0)** 의 `bcb-prior-test` 측정값이며, 128B 메시지 기준이다.
> 절대 처리량은 메시지 크기·prior·하드웨어에 좌우된다. (`docs/mmap_prior.md`에 별도 회차 432/445/569
> msgs/s 기록 있음 — 머신·빌드 차이로 회차마다 다름; 위는 이번 실측.)

---

## 7. 스레드 안전성 / 무결성

| 항목 | 값 | 근거 | [등급] |
|------|----|----|--------|
| 스레드 안전 모델 | **per-instance.** 각 `BcbEncoder`/`BcbDecoder`가 자기 인스턴스 상태(window/schema pos) 소유. prior·LUT는 읽기전용 공유 → 서로 다른 핸들을 여러 스레드가 동시 사용 안전. **단일 핸들의 동시 사용은 불가** | `bcb_api.c:7` 주석, `BtV3Reader` per-instance | SRC |
| threads-test 통과 | **통과.** 8스레드 × 4000 iters, landmark·structural prior 모두 0 failures | `make threads-test` → `PASSED (0 thread failures)` ×2 | MEAS+CI |
| 무결성 체크 방식 | **CRC32 (IEEE, bitwise — 테이블 없음→lazy-init race 없음)** | `bcb_api.c:26-27` | SRC |
| CRC 기본값 | **on** (encoder `crc_on=1` 기본). 컨테이너 tag bit0로 표기, 디코더 자동 감지 | `bcb_api.c:154` `e->crc_on=1`; tag `BCB_TAG_CRC` | SRC |
| CRC off 가능 | `bcb_encoder_set_checksum(e,0)` (4B 절약, 최대 압축비) | `bcb_api.c:165` | SRC |
| 손상 시 동작 | CRC 불일치/컨테이너 손상 시 **`BCB_ERR_CORRUPTED`(-3)** 반환 (크래시 아님) | `bcb_api.c:118`; `make api-test` corruption 케이스 통과 | SRC+CI |
| prior 불일치 검출 | prior-id(SHA-256 앞 16B) 임베드 시 디코더가 **`BCB_ERR_PRIOR_ID_MISMATCH`(-5)** | `bcb_api.c`, `include/bcb.h:74`; `make api-test` 통과 | SRC+CI |
| 무결성 한계 | CRC32는 **우발적 손상 검출**용. **적대적 위변조 방지 아님** | `bcb_api.c` 설계 | SRC |

---

## 신뢰도 분류 요약

**CI 검증됨 (빌드·테스트 자동화):**
- Linux x86-64 / Windows x86-64 빌드 + 무손실 round-trip·API·thread·msgbench 테스트
- macOS 빌드 + sanity ctest
- threads-test 8스레드 무손실, api-test 손상·prior-id 검출
- Python wheel CI 빌드 (Linux/macOS/Windows, cp38–313)

**소스 확인됨:**
- C API 0.2.0 및 API 표면 / cffi 바인딩 방식
- per-instance 스레드 모델, CRC32 기본 on·손상시 `BCB_ERR_CORRUPTED`
- 분포 계산 경로 무할당·정수·libm-free
- 코어 외부 라이브러리 의존 0 (brotli/zstd는 벤치 전용)

**실측됨 (이 머신, x86-64 gcc 13.3.0):**
- `libbcb.a` 67,442 B / `libbcb.so.0.2.0` 54,536 B
- MCU prior 모델 3.56 MB(고정), 데스크톱 94.5 MB(동적, 30KB 코퍼스)
- 코어 `.text` 27,014 B (MCU 빌드, x86-64 codegen)
- 처리량 185/190/255 msgs/s (in-mem/mmap/MCU, 128B 메시지)
- `libbcb.so`가 libm 링크함 (`ldd` 확인) — "libm-free"는 v3 정수 분포 경로 한정

**미측정·미검증 (구매자가 찌르면 "아직 안 했다"가 정답):**
- ARM32/ARM64/aarch64 **네이티브** 빌드·테스트 (wheel 설정만, QEMU 미설정)
- 실제 MCU 칩(ESP32/RP2040) 온디바이스 flash/RAM/속도
- 타깃 MCU(ARM) `.text` 크기, 스택 사용량
- encode와 decode **분리** 속도, MB/s 단위
- `.dll`/`.dylib` 파일 바이트 크기
