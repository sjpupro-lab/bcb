# BCB on MCU — 임베디드 가이드

v3 정수화(단계 1~4) 완료. BT 분포 hot path 에 `double`·libm 없음. `-DBCB_MCU` 로 소형 빌드.

## 정수화 현황

- **range coder** (`ce_compress.c`) — 32-bit 정수만. MCU 호환.
- **v3 BT** (`src/v3_integer_bt/btv3.c`) — 분포 hot path 정수 전용
  (log-domain: `EXP_LOG2`/`CONF_LOG2`/`EXP2_FRAC` 정수 LUT). LUT 는 init 에서 정수
  `log2`/`exp2`(integer sqrt 기반) 로 생성 — **libm(`<math.h>`) 불필요**.
- **v4 보조채널** (`src/v4_aux_channel/aux_channel.c`) — blend 정수화 (Q16 α, 정수 prior 폭).

## 메모리 footprint (`make meminfo` → `bt_v3_footprint()`)

이 표의 숫자는 **학습된 prior 모델 테이블이 차지하는 RAM**(bt pool/slot, ctx pool/slot,
bloom, 정수 LUT)이다. **코드(flash/.text) 크기도, 스택도, 학습 시점 임시 메모리도 아니다.**
`make meminfo` 가 `bt_v3_footprint()` 로 컴파일된 설정의 테이블 용량을 합산해 출력한다.

- **desktop (동적 빌드)** 은 학습량에 따라 pool 이 `realloc` 으로 자라므로 **고정값이 아니다.**
  예: 30KB 코퍼스 학습 시 pool 이 1M 엔트리까지 자라 **합계 ≈ 94.5 MB** (이 레포에서 `make
  meminfo` 실측), 대형 코퍼스로 8M 엔트리까지 자라면 아래 표의 546.5 MB. 즉 desktop 열은
  **8M-pool 운영점**을 보여주는 예시다.
- **MCU (`-DBCB_MCU`)** 는 pool 을 **고정**(성장 없음)하므로 합계가 코퍼스와 무관하게 **고정**이다.
  컴파일 상수에서 정확히 도출된다: 65536×36 + 131072×4 + 16384×40 + 32768×4 + 262144/8 +
  8192×4 = **3,735,552 B = 3.56 MB** (`btv3.c` `#ifdef BCB_MCU` 블록 참조).

| 구조 | desktop (동적, 8M-pool 예시) | MCU (`-DBCB_MCU`, 고정) |
|------|---------|-------------------|
| bt pool | 288 MB (8M×36B) | 2.25 MB (64K×36B) |
| bt slot | 64 MB | 0.50 MB (128K) |
| ctx pool | 160 MB (4M×40B) | 0.62 MB (16K×40B) |
| ctx slot | 32 MB | 0.12 MB (32K) |
| bloom | 2 MB (16M bit) | 0.03 MB (256K bit) |
| LUTs | 0.5 MB (65536×2) | 0.03 MB (4096×2) |
| **합계** | **546.5 MB** (학습량 비례; 30KB→94.5MB) | **3.56 MB** (고정) |

> **on-device 미측정:** 위 3.56 MB 는 모델 테이블의 RAM 예산이다. 특정 MCU 보드에서의
> end-to-end flash/.text 크기·런타임 RSS·전력·지연은 아직 측정하지 않았다(bare-metal 포팅 TODO).
> 코덱 코드 자체(flash)는 수십 KB 수준으로 추정되나 타깃 측정 전까지는 추정치다.

- MCU 빌드는 LUT 해상도도 축소(CONF_LOG2/EXP2_FRAC 4096 entries). 정밀도 약간↓, 무손실 유지.
- ESP32 (4MB Flash, PSRAM) — pool 을 PSRAM 에 두면 3.56MB 적합.
- RP2040 (2MB Flash, 264KB SRAM) — pool 을 외부 flash/PSRAM 에. SRAM 단독엔 추가 축소 필요
  (BT_POOL/슬롯을 더 줄이거나 ctx pool 축소). 컴파일 매크로로 조정 가능.

## 무손실/압축비 (`make meminfo`, 30KB 학습 / 4KB 발췌)

| 빌드 | 압축비 | entries | lossless |
|------|--------|---------|----------|
| desktop | 4.55× | 617,781 | yes |
| MCU | 3.06× | 65,536 (pool full) | yes |

MCU 는 pool 상한(64K) 에서 포화해 학습이 제한되어 압축비가 낮지만 **무손실은 동일**.
메모리↔압축비 트레이드오프. 더 큰 pool(매크로 조정)로 압축비 회복 가능.

## 빌드

```sh
make meminfo          # desktop + MCU footprint·무손실 점검
cc -DBCB_MCU -Isrc/v0_baseline -Isrc/v3_integer_bt ... btv3.c   # MCU 설정 컴파일
```

`-DBCB_MCU` 가 BT_POOL/슬롯/bloom/ctx/LUT 크기를 일괄 축소한다 (`btv3.c` 상단 참조).

## 코어 의존성 (소스 확인됨)

- **핫패스 malloc/free 없음.** `malloc`/`realloc`/`calloc` 은 학습(`bt_grow`/`ctx_grow`),
  init(`build_luts`/`bt_v3_init`), prior 로드/검증 경로에만 있다. encode/decode 분포 계산
  (`bt_v3_distribution_r`, `ctx_find_rd`)은 할당하지 않는다 (`btv3.c` 주석·코드 참조).
- **libm(`<math.h>`) 없음.** LUT 는 정수 fixed-point 로 생성한다. `<stdio.h>` 도 코어
  (`btv3.c`)에는 없다. 다만 `<stdlib.h>`(malloc, 학습/로드용)·`<string.h>` 는 쓴다.
- **mmap 없이 메모리에서 prior 로드 가능.** `bcb_prior_from_memory()`(`include/bcb.h`,
  `bcb_api.c` → `bcb_prior_from_buffer`)가 in-memory 이미지를 복사·소유해 연다. 펌웨어에
  prior 를 베이크해 쓰는 경로다. CI `prior-equiv`(in-memory == mmap, bit-identical) +
  `test_api` 로 검증된다.

## emCompress / Oodle 와의 위치 (오해 방지)

SEGGER emCompress 는 **디컴프레션 임시 RAM ~2KB, 스택 <512B, static RAM 0** 을 광고한다 —
이는 LZ 계열이 *블록 내부* 모델만 쓰기 때문이다. BCB 는 설계가 다르다: 양쪽이 **공유 학습 prior**
를 보유하고, 그 prior 가 곧 3.56 MB(MCU 빌드)짜리 **읽기 전용 모델**이다(flash/PSRAM 상주 가능,
메시지당 인코더 작업 상태는 소형). 즉 BCB 는 *큰 공유 모델*을 대가로 작은 메시지에서 더 높은
압축비를 얻는 트레이드오프이며, **emCompress 의 sub-2KB RAM 과 동급이라고 주장하지 않는다.**
emCompress 가 부풀리는 작은 per-packet 레코드 구간이 BCB 의 타깃이다.
