# BCB on MCU — 임베디드 가이드

v3 정수화(단계 1~4) 완료. BT 분포 hot path 에 `double`·libm 없음. `-DBCB_MCU` 로 소형 빌드.

## 정수화 현황

- **range coder** (`ce_compress.c`) — 32-bit 정수만. MCU 호환.
- **v3 BT** (`src/v3_integer_bt/btv3.c`) — 분포 hot path 정수 전용
  (log-domain: `EXP_LOG2`/`CONF_LOG2`/`EXP2_FRAC` 정수 LUT). LUT 는 init 에서 정수
  `log2`/`exp2`(integer sqrt 기반) 로 생성 — **libm(`<math.h>`) 불필요**.
- **v4 보조채널** (`src/v4_aux_channel/aux_channel.c`) — blend 정수화 (Q16 α, 정수 prior 폭).

## 메모리 footprint (`make meminfo`, 실측)

| 구조 | desktop | MCU (`-DBCB_MCU`) |
|------|---------|-------------------|
| bt pool | 288 MB (8M×36B) | 2.25 MB (64K×36B) |
| bt slot | 64 MB | 0.50 MB (128K) |
| ctx pool | 160 MB (4M×40B) | 0.62 MB (16K×40B) |
| ctx slot | 32 MB | 0.12 MB (32K) |
| bloom | 2 MB (16M bit) | 0.03 MB (256K bit) |
| LUTs | 0.5 MB (65536×2) | 0.03 MB (4096×2) |
| **합계** | **546.5 MB** | **3.56 MB** |

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
