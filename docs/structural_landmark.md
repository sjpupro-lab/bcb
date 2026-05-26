# Structural Landmark — 위치(position) 기반 landmark (v6)

v5 landmark 는 *byte sequence* hit 라 텍스트형 반복 데이터에서만 효과(HTTP +35%, IoT 0%).
v6 는 hit 을 **고정 레코드 내 위치(position)** 로 확장해 binary record(IoT/Modbus/CAN)에서도
압축한다. 호시님 통찰: "구조(structure)가 곧 prior". 재현: `make structbench`.

## 아이디어

고정 크기 R 레코드에서 위치 p 의 바이트는 통계가 뚜렷하다 — magic/version 은 상수,
seq/timestamp 는 단조 증가, 센서값은 느린 변화. 두 모델:
- **pos-byte**: `P(byte | position p)` — 상수/저범위 자리에 강함.
- **pos-delta**: `P(byte − prev_record[p] | position p)` — 카운터·느린 신호에 강함(델타가 작음).
- **hybrid**: 자리마다 train entropy 가 낮은 모드를 선택.

## PR-1 측정 (잠재력 검증) — `make structbench`

train 50KB / test 50KB, entropy 추정(cross-entropy bits). **실제 코딩 바이트·무손실은 PR-2/3.**

| 시나리오 | R | base | pos-byte | pos-delta | hybrid | hybrid 이득 | delta 자리 |
|---|---|---|---|---|---|---|---|
| binary_record | 32 | 1.48× | 1.81× | 5.48× | **5.49×** | **+73.0%** | 18/32 |
| modbus | 25 | 1.62× | 1.86× | 6.72× | 5.49× | **+70.6%** | 13/25 |
| canbus | 16 | 2.59× | 3.22× | 3.96× | **4.36×** | +40.6% | 3/16 |
| iot_packets | 18 | 1.36× | 1.71× | **1.90×** | 1.73× | +21.1% | 2/18 |

비교: gzip/zstd/brotli 는 이런 작은 binary 레코드에서 ~0.95×(오히려 키움).

## 정직한 해석

- **단일 스트림 정형 레코드에서 도약**: binary_record +73%, modbus +71%. 단조 카운터/느린 레지스터를
  delta 가 거의 0 비트로 압축(5.5×, 6.7×). 구조 prior 의 진짜 위력.
- **canbus +40.6%**: 여러 CAN ID 가 interleave 되어 자리 의미가 ID마다 달라(위치 정합 약화) delta 단독은
  3.96×, hybrid(자리별 best)가 4.36× 로 더 낫다.
- **IoT 목표(1.31→≥1.8×)는 pos-delta(1.90×)로 달성**. 단:
  - 원 기획 "position-aware 2.03× / hybrid 2.09×" 는 **재현되지 않음** — 실측 pos-delta 1.90×, pos-byte 1.71×.
    IoT 합성기가 패킷마다 device 를 무작위 선택(multi-device interleave)해 per-position delta 연속성이
    약하기 때문. 그래도 magic/version/저범위 상위바이트 덕에 +40% (base 1.36→delta 1.90).
  - **hybrid(1.73×) < pos-delta(1.90×)**: 자리별 mode 를 train self-entropy 로 고르는데, IoT interleave 에서
    train/test mode 가 어긋나 일부 자리에서 byte 를 골라 손해. → **PR-3 에서 mode 선택 개선**(held-out 검증
    또는 byte+delta 결합). 현재도 pos-delta 강제면 목표 달성.

## 시나리오 합성기 (record-aligned, 결정적)

- `tests/scenarios/binary_record.py` (32B custom schema: const/counter/slow-walk/random 혼합)
- `tests/scenarios/modbus.py` (25B Modbus/TCP read-holding-registers 응답)
- `tests/scenarios/canbus.py` (16B CAN 로그, 여러 ID interleave)
- 기존 `iot_packets.py` (18B) 재사용.

## 다음 단계 (PR-2 / PR-3)

- **PR-2**: prior 파일에 record schema section(record_size + 자리별 정수 양자화 cum) 추가,
  인코더/디코더가 위치 p 에서 해당 cum 사용(델타는 prev_record[p] 만큼 순환 시프트한 cum 으로 — actual
  byte 를 그대로 코딩하면서 델타 효과). 정수 양자화라 **무손실 구조적 보장**. IoT/binary/modbus/canbus
  round-trip + 텍스트 시나리오 회귀 없음 검증.
- **PR-3**: 자리별 byte/delta mode 자동 선택 개선(위 hybrid 결함 교정) + 추가 측정.
