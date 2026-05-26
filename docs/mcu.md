# BCB on MCU — 임베디드 가이드 (초안)

> 정수 전용 빌드(v3)가 완료되면 본 문서를 확정한다. 현재는 방향만 기록한다.

## 현황

- **range coder** (`ce_compress.c`) — 이미 32-bit 정수만 사용. MCU 호환.
- **BT predict** (`bt_model.c` `predict_byte`) — `exp`, `pow`, `double` 사용.
  v3 에서 정수 LUT 로 대체 필요 (`EXP_LUT[24]`, `CONF_LUT[256]`).

## 메모리 (현재 full 빌드)

| 구조 | 크기 |
|------|------|
| `BtTable.pool` (8M entries × ~36B) | ~288 MB |
| `g_ctx_pool` (4M entries × ~40B) | ~160 MB |
| bloom (1<<22 bits) | 512 KB |
| → 합계 | **≈ 450 MB** — MCU 부적합 |

## MCU 빌드 목표 (v3)

| 구조 | 축소안 | 크기 |
|------|--------|------|
| BT pool | 64K entries | ~2 MB |
| bloom | 256K bits | 32 KB |
| ctx pool | 16K entries | ~640 KB |

- ESP32 (4MB Flash, ~520KB SRAM) — pool 을 PSRAM 에 배치 시 가능성.
- RP2040 (2MB Flash, 264KB SRAM) — 더 작은 pool + 외부 flash 고려.

## TODO (v3)

- [ ] double → 정수 LUT 치환, 동일 압축본(round-trip) 검증
- [ ] 컴파일 타임 매크로로 pool 크기 선택 (`-DBCB_MCU`)
- [ ] libm 의존성 제거
