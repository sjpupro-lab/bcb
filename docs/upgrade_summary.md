# BCB 업그레이드 사양 정리 / Upgrade Spec Summary

기준 커밋: `main` `c3ad3d1` (PR #47 · #48 · #49 반영)
측정 머신: x86-64 Linux CI 러너, gcc, `-O2`
신뢰도 표기: 🟢실측 · 🔵CI검증 · ⚪소스확인 · ⚫미측정

> 이 문서는 이번 업그레이드 사이클(3개 PR)에서 **무엇이 바뀌었고**, 그 결과 **현재 제품
> 사양이 어떤지**를 한 장으로 정리한 것이다. 상세 항목별 근거는 `docs/spec_sheet.md` 참조.

---

## 1. 이번 업그레이드 요약 (3개 PR)

| PR | 기능 | 핵심 효과 | 무손실/호환 | 상태 |
|---|---|---|---|---|
| **#47** | structural delta-symbol fast path (`BCB_TAG_SDELTA`) | 레코드형 데이터에 자리별 사전계산 cum 적용 — BT 분포 탐색 제거 | round-trip 무손실 | ✅ 머지 |
| **#48** | btv3 분포 핫패스 최적화 | 미관측 byte 기여를 레벨당 1회만 계산 (이전엔 256회 반복) | **bit-identical** (바이트 동일) | ✅ 머지 |
| **#49** | `BCB_MCU_NO_DIV` — 하드웨어 divider 없는 MCU용 코덱 | 64-bit `/` 를 shift / shift-subtract 로 대체 | **bit-identical** (플래그 off 시 무변화) | ✅ 머지 |

세 패치 모두 **출력 호환성을 깨지 않는다** — #48·#49 는 바이트 단위로 동일한 압축 스트림을
내고, #47 은 새 태그를 추가하되 round-trip 무손실이다.

---

## 2. 업그레이드로 바뀐 수치 (Before → After)

### 2-1. 인코드 속도 (텍스트/landmark 경로) — #48
btv3 분포 핫패스 최적화. **바이트 동일 출력**을 유지하면서 빨라졌다.

| 시나리오 | 속도 향상 (base prior, rss-mmap) | 비고 |
|---|---|---|
| http_headers | **2.92×** | 미관측 byte 많을수록 이득 큼 |
| mqtt_messages | **2.07×** | |
| iot_packets | **1.37×** | |

- 검증: main 대비 mqtt/http/iot 압축 출력 **byte-identical (cmp 3/3)**, `make prior-equiv` 5/5 무손실.

### 2-2. 레코드형 데이터 측정 — #47 + spec 정정
레코드형(structural) 데이터는 **schema prior** 로 측정해야 맞다. structural codec 은
자리별 사전계산 cum 만 읽어(BT 탐색 없음) 텍스트/landmark 경로의 **~30×** 속도를 낸다.

| 레코드 시나리오 | 압축비(재현) | 인코드 msgs/s @128B (대표값) |
|---|---|---|
| modbus (rec 25) | 6.7× | ~38,000 |
| canbus (rec 16) | 4.3× | ~42,000 |
| iot (rec 18) | 1.24× | ~30,000 |
| binary_record (rec 32) | 0.99× | ~29,000 |

> **정정:** 이전 spec 의 "iot 959 msgs/s landmark" 는 **데이터-기능 짝 오류**였다(레코드
> 데이터를 landmark 로 측정 → context 불일치로 ~300 msgs/s 폴백). iot 는 레코드형이므로
> schema 로 재야 하며 실측 ~30k msgs/s. 단 structural 은 속도를 위해 압축비를 희생할 수
> 있다(iot structural 1.24× vs 같은 데이터 landmark 1.99×).

### 2-3. MCU 이식성 — #49
하드웨어 divider 가 없는 MCU(Cortex-M0/M0+, RV32I 등)에서 64-bit `/` 가 libgcc 호출
(`__udivdi3` / `__aeabi_uldivmod`)로 떨어지는 것을 제거하는 컴파일 옵션.

- power-of-two 분모(`CEC_RC_SCALE = 1<<14`, structural `cum[256]=16384`) → **shift**
- 가변 분모(decode `target`) → 인라인 radix-2 **shift-subtract** 분배기(`bcb_divq64`)
- **플래그 미정의 시 원본 `/` 그대로** → 기본 빌드 무변화

검증:
- `make nodiv-test`: **4,200,150 케이스, 0 mismatch** (분배기 == 하드웨어 `/`)
- NO_DIV lib vs 일반 lib **end-to-end byte-identical**: 텍스트(mqtt/http) + 레코드(binary/iot/modbus/canbus) 전부 동일, round-trip 0 fail
- MCU 조합(`-DBCB_MCU -DBCB_MCU_NO_DIV`): 컴파일 + MCU 코덱 equiv `bit-identical=yes lossless=yes`

---

## 3. 현재 제품 사양 스냅샷 (업그레이드 반영)

### 메모리 / 풋프린트 🟢실측
| 항목 | 값 |
|---|---|
| 디코드 peak 스택 | **1.53 KB** (1,568 B) |
| 인코드 peak 스택 | **1.61 KB** (1,648 B) |
| codec 핸들 heap (`BcbCodec`) | 33,832 B (scratch 31,680 포함; codec당 1회) |
| prior 모델 (MCU 고정 빌드) | **3.56 MB** (읽기전용, flash/PSRAM 배치) |
| 정수 LUT | 32 KB (1회 생성, 읽기전용) |

### 라이브러리 크기 (x86-64) 🟢실측
| 항목 | 값 |
|---|---|
| static `libbcb.a` | **59,388 B** |
| shared `libbcb.so.0.2.0` | **49,792 B** |
| MCU 코어 `.text` | **26,914 B** |

### 의존성 🟢실측
- 외부 서드파티 라이브러리(코어): **0개**
- 공유 라이브러리 외부 의존: **libc 만 (libm 0)**
- 인코드/디코드/prior 빌드 전부 정수·사칙연산 (초월함수 호출 0)
- **#49 이후 추가 강점:** `BCB_MCU_NO_DIV` 로 하드웨어 나눗셈기까지 불필요 → divider 없는 MCU 이식 가능

### 압축비 (머신 무관, 재현) 🟢실측
| 데이터형 | 권장 prior | 압축비 |
|---|---|---|
| 텍스트 (http) | landmark | **10.6×** |
| 텍스트 (mqtt / rpc) | landmark | 6.0× |
| 텍스트 (log) | landmark | 4.9× |
| 레코드 (modbus) | schema | 6.7× |
| 레코드 (canbus) | schema | 4.3× |

### 플랫폼 / 무결성
| 항목 | 상태 |
|---|---|
| Linux x86-64 · Windows x86-64 | 🔵CI 빌드+무손실 검증 |
| macOS (universal) | 🔵CI 빌드 |
| MCU 빌드(`-DBCB_MCU`[`+NO_DIV`]) | 🟢호스트 컴파일·무손실 (온디바이스 ⚫미측정) |
| 스레드 모델 | per-instance, 8스레드 동시 무손실 0 failure |
| 무결성 | CRC32 (IEEE) 기본 on, 손상 시 `BCB_ERR_CORRUPTED` |

---

## 4. 포지셔닝 (정직히)

- **강점:** 작은 패킷(≤수백 B) 압축비 + 초소형 풋프린트(스택 1.5 KB, libc·divider 외 의존 0).
  엣지 텔레메트리(초당 수백~수천 패킷/코어)에 적합.
- **속도 한계:** 텍스트/landmark 경로는 바이트마다 256-심볼 분포를 계산하므로 LZ
  (brotli/zstd, µs)보다 100×+ 느리다. 고throughput 스트리밍엔 부적합.
- **레코드형 예외:** structural codec 은 ~30k–42k msgs/s 로 훨씬 빠르나, 압축비를 일부 희생.
- **수치 사용 주의:** 절대 msgs/s 는 머신·부하 의존(±15%)이라 머신 명시 없이 인용 금지.
  **압축비는 재현되므로** 공개 자료엔 압축비 위주로 인용 권장.
