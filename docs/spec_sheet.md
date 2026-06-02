# BCB 제품 사양표 / Product Spec Sheet

기준 커밋: `main` (PR #42·#43·#44·#45 반영 — v0 코덱 제외, decode 스택 축소, 라이브러리 libm-free).
측정 머신: **x86-64, gcc 13.3.0, Ubuntu 24.04.4 LTS**.

신뢰도 표기: 🟢실측 · 🔵CI검증 · ⚪소스확인 · ⚫미측정(공개자료 미사용)

모든 값은 (a) 소스 코드, (b) 빌드 산출물, (c) 실제 측정 명령 출력 중 하나를 근거로 한다.
"미측정" 항목은 빈칸 대신 정직하게 미측정으로 표기하며 공개 자료에 수치로 쓰지 않는다.

---

## 1. 지원 플랫폼 / 아키텍처

| 플랫폼 | 상태 | 신뢰도 | 근거 |
|---|---|---|---|
| Linux x86-64 | 빌드 + 무손실 테스트 통과 | 🔵CI검증 | `msgbench.yml`: test/api-test/threads-test/prior-equiv |
| Windows x86-64 (MSVC) | 빌드 + DLL round-trip ctest | 🔵CI검증 | `windows.yml` |
| macOS (universal) | 빌드 + sanity ctest | 🔵CI검증 | `release.yml` build-native |
| ARM64 / aarch64 | wheel 빌드 설정만 존재(QEMU 없음) | ⚫미측정 | 네이티브 CI·실행 없음 |
| ARM32 / armv7 | 미지원 | — | 워크플로/설정 전무 |
| MCU 빌드 (`-DBCB_MCU`) | 호스트에서 컴파일·footprint·무손실 round-trip | 🟢실측 | `make meminfo` (호스트 codegen) |
| 실제 MCU 칩 (ESP32/RP2040) | 설계 타깃, 온디바이스 미측정 | ⚫미측정 | `docs/mcu.md` (bare-metal 포팅 TODO) |

> 단언 가능: **Linux x86-64 · Windows x86-64 CI 검증, macOS 빌드.** ARM/MCU 실칩은 "설계 타깃".

## 2. 런타임 메모리

| 항목 | 값 | 신뢰도 | 근거 |
|---|---|---|---|
| 디코드 peak 스택 | **1,568 B (1.53 KB)** | 🟢실측 | `-fstack-usage`, 최심 호출 체인 합 |
| 인코드 peak 스택 | **1,648 B (1.61 KB)** | 🟢실측 | `-fstack-usage` |
| 인코더/디코더 핸들 heap (`BcbCodec`) | **33,832 B** (분포 scratch 31,680 포함; codec당 1회, 패킷마다 아님) | 🟢실측 | `sizeof` |
| `CecDecoder` heap | 56 B | 🟢실측 | `sizeof` |
| 정수 LUT (1회 생성·읽기전용) | 32 KB | ⚪소스확인 | `build_luts` (MCU) |
| prior 모델 (MCU 고정 빌드) | **3.56 MB** (3,735,552 B, 읽기전용, flash/PSRAM 배치 가능) | 🟢실측 | `make meminfo` → `bcb-meminfo-mcu` TOTAL |

## 3. 라이브러리 크기 (x86-64)

| 항목 | 값 | 신뢰도 | 근거 |
|---|---|---|---|
| static `libbcb.a` (CMake) | **59,388 B** | 🟢실측 | `ls -l` |
| shared `libbcb.so.0.2.0` | **49,792 B** | 🟢실측 | `ls -l` |
| MCU 코어 `.text` | **26,914 B** | 🟢실측 | `size` (`-DBCB_MCU` 객체 합) |
| `.dll` / `.dylib` 바이트 | — | ⚫미측정 | CI 빌드되나 크기 미기록 |

## 4. 의존성

| 항목 | 값 | 신뢰도 | 근거 |
|---|---|---|---|
| 외부 서드파티 라이브러리 (코어) | **0개** (brotli/zstd는 벤치 도구 전용) | ⚪소스확인 | `src/` 에 없음 |
| 공유 라이브러리 외부 의존 | **libc 만** (libm 0) | 🟢실측 | `ldd libbcb.so.0.2.0` → `libc.so.6` 만 |
| libm 의존 | **없음** — 인코드/디코드/prior 빌드 전부 정수·사칙연산만 (초월함수 호출 0) | 🟢실측 | `nm -D` undefined math 심볼 0 |
| 표준 헤더 (코어 btv3) | `<stdlib.h>`,`<string.h>`,`<stdint.h>` — `<stdio.h>`·libm 없음 | ⚪소스확인 | `btv3.c` |

## 5. API / 바인딩

| 항목 | 값 | 신뢰도 | 근거 |
|---|---|---|---|
| C 공개 API 버전 | **0.2.0** | ⚪소스확인 | `include/bcb.h` `BCB_VERSION_*` |
| 바인딩 방식 | cffi (C 코어 직접 컴파일, 별도 .so dlopen 없음) | ⚪소스확인 | `bcb_build.py` |
| Python wheel — Python | cp38–cp313 | ⚪소스확인 | `pyproject.toml` |
| Python wheel — 플랫폼 (CI 빌드) | Linux · macOS · Windows | 🔵CI검증 | `release.yml` build-wheels |
| Python wheel — aarch64/arm64 | 설정만, 실제 산출 미확인 | ⚫미측정 | QEMU 스텝 없음 |

## 6. 인코드 / 디코드 속도

| 항목 | 값 | 신뢰도 | 근거 |
|---|---|---|---|
| 인코드 처리량 @128B (x86-64) | 193 (in-mem) / 197 (mmap) / 240 (MCU 빌드) msgs/s | 🟢실측 | `make prior-rss` (타이머=인코드 루프) |
| 디코드 전용 속도 (msgs/s, MB/s) | — | ⚫미측정 | `prior-rss`는 인코드만 측정 |

## 7. 스레드 안전성 / 무결성

| 항목 | 값 | 신뢰도 | 근거 |
|---|---|---|---|
| 스레드 모델 | per-instance (핸들마다 상태·scratch; prior 읽기전용 공유) | ⚪소스확인 | `BcbCodec`/`BtV3Reader.scratch` |
| 동시 무손실 통과 | 8스레드 × 4000, landmark+structural, 0 failures | 🔵CI검증🟢실측 | `make threads-test` |
| 무결성 | CRC32 (IEEE), 기본 on | ⚪소스확인 | `bcb_api.c` `crc_on=1` |
| 손상 시 | `BCB_ERR_CORRUPTED` 반환 (크래시 없음) | 🔵CI검증 | `api-test` 손상 케이스 |

---

## 공개 자료에 안심하고 쓸 핵심 수치 (🟢실측)

- 디코드 스택 **1.53 KB**, 인코드 스택 1.61 KB
- prior 모델 **3.56 MB** (읽기전용, flash/PSRAM)
- 라이브러리 static **59 KB** / shared **49.8 KB** / MCU 코어 .text **26.9 KB**
- 외부 의존성 **libc 만 (libm 0, 서드파티 0)**
- per-instance 스레드 안전, CRC32 기본 on

## 공개 자료에 쓰면 안 되는 것 (미측정)

- 디코드 **전용** 속도 (측정된 건 인코드 처리량)
- ARM / 실제 MCU 칩 동작
- `.dll`/`.dylib` 바이트 크기, ARM codegen `.text`
