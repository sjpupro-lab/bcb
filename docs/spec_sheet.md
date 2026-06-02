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

측정: `bcb-prior-test rss-mmap <corpus> <prior> --msgs 5000`, @128B, 1스레드, 이 CI 러너.
**데이터-기능 짝**을 맞춰 측정한다 — 텍스트형은 landmark prior, 레코드형은 schema(structural)
prior. 짝이 틀리면(예: 레코드 데이터를 landmark 로) 매 바이트 full BT 분포로 폴백해 수백 msgs/s 로
떨어진다(아래 "측정 주의" 참조). **압축비(ratio)는 머신과 무관히 재현되지만 절대 msgs/s 는
머신·부하 의존**이라 대표값(반복 측정 중앙값)으로 적는다.

| 항목 | 압축비(재현) | 인코드 msgs/s (대표값, 머신의존) | 신뢰도 |
|---|---|---|---|
| **텍스트형 + landmark prior** (권장 짝) | mqtt 6.0× · http 10.6× · log 4.9× · rpc 6.0× | mqtt ~1,000 · http ~2,200 · log ~960 · rpc ~1,120 | 🟢실측 |
| 텍스트형 + base prior (landmark 없음) | mqtt 5.1× · http 7.0× · log 4.3× · rpc 5.0× | mqtt ~620 · http ~730 · log ~560 · rpc ~470 (landmark 의 ~0.5–0.6×) | 🟢실측 |
| **레코드형 + schema prior** (권장 짝) | binary 0.99× · iot 1.24× · modbus 6.7× · canbus 4.3× | binary ~29k · iot ~30k · modbus ~38k · canbus ~42k | 🟢실측 |
| 디코드 @128B | ≈인코드와 대칭 (같은 분포 계산이 병목) | ≈인코드 | 🟢실측 |
| 다중 스레드 | — | per-instance, 선형 확장 (8스레드 ≈ 8×) | ⚪소스확인 (`make threads-test`) |

> **측정 주의 (정직히):**
> - **레코드형 데이터는 schema prior 로 측정해야 한다.** structural codec 은 자리별 사전계산 cum 만
>   읽어(BT 분포 탐색 없음) **텍스트/landmark 경로의 ~30×** 인 30k–42k msgs/s 를 낸다. 같은 iot
>   데이터를 landmark prior 로 재면 context 가 안 맞아 ~300 msgs/s 로 떨어진다 — 짝이 틀린 측정이다.
>   (이전 spec 의 "iot 959 msgs/s landmark" 는 이 짝 오류였고 재현되지 않아 정정한다.)
> - structural 의 빠른 속도는 **압축비를 희생**한 결과일 수 있다(iot 1.24× vs 같은 데이터 landmark
>   1.99×). 속도냐 압축비냐는 데이터·요구사항에 따라 고른다.
> - 텍스트/landmark 경로(바이트마다 256-심볼 BT 분포 계산)는 LZ(brotli/zstd, µs)보다 **100×+ 느리다.**
>   강점은 속도가 아니라 작은 패킷 압축비·풋프린트다. 엣지 텔레메트리(초당 수백~수천 패킷/코어)엔
>   충분하나 고throughput 스트리밍엔 부적합.
> - btv3 분포 핫패스 최적화(미관측 byte 기여를 레벨당 1회 계산)로 텍스트/landmark 경로가 **bit-identical
>   하게 ~1.4–2.9× 빨라졌다**(시나리오별, `make prior-equiv` 무손실·바이트동일 확인).

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
- **압축비**(머신 무관 재현): 텍스트형 landmark mqtt 6.0× / http 10.6×, 레코드형 schema modbus 6.7× / canbus 4.3×

## 공개 자료에 쓰면 안 되는 것 (미측정/주의)

- **절대 msgs/s 를 머신 명시 없이** 쓰지 말 것 (CI 러너 부하에 ±15% 변동; 압축비는 재현됨)
- 데이터-기능 짝이 틀린 속도 (예: 레코드 데이터를 landmark 로 잰 값)
- ARM / 실제 MCU 칩 동작
- `.dll`/`.dylib` 바이트 크기, ARM codegen `.text`
