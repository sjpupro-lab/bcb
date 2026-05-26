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

## 측정 (잠재력 검증) — `make structbench`

train 50KB / test 50KB, entropy 추정(cross-entropy bits). **실제 코딩 바이트·무손실은 PR-2.**
hybrid 의 자리별 mode 는 **held-out 검증**(train 80% 적합 / 20% 로 mode 결정)으로 고른다.

| 시나리오 | R | base | pos-byte | pos-delta | hybrid | hybrid 이득 | delta 자리 |
|---|---|---|---|---|---|---|---|
| binary_record | 32 | 1.48× | 1.81× | 5.48× | **5.49×** | **+73.0%** | 19/32 |
| modbus | 25 | 1.62× | 1.86× | **6.72×** | 5.49× | +70.6% | 13/25 |
| canbus (interleave) | 16 | 2.59× | 3.22× | 3.96× | **4.36×** | +40.6% | 4/16 |
| **iot (single device)** | 18 | 1.33× | 1.48× | **6.45×** | 4.79× | +72.2% | 10/18 |
| iot (8-dev interleave) | 18 | 1.36× | 1.71× | 1.90× | **2.04×** | +33.1% | 6/18 |

비교: gzip/zstd/brotli 는 이런 작은 binary 레코드에서 ~0.95×(오히려 키움).

## 정직한 해석

- **단일 스트림 정형 레코드에서 도약**: binary_record +73%, modbus +71%, **per-device IoT pos-delta 6.45×**.
  단조 카운터/느린 센서를 delta 가 거의 0 비트로 압축. 구조 prior 의 진짜 위력.
- **IoT 는 배치 방식이 전부를 가른다**:
  - **per-device stream**(각 디바이스/연결이 자기 스트림) → **pos-delta 6.45×** (base 1.33×). 이것이 BCB 의
    진짜 IoT 잠재력 — 단일 device entropy 한계.
  - **다중 device interleave**(공유 게이트웨이) → hybrid 2.04× 에 그친다. 직전 레코드가 다른 device 라
    per-position delta 연속성이 깨지기 때문. 원 기획 "2.03×" 는 이 interleave 구간 값에 해당하고,
    "+55%/5×+" 는 **per-device 환경에서만** 성립한다.
- **canbus +40.6%**: 여러 CAN ID interleave → 자리 의미가 ID마다 달라 hybrid(4.36×)가 delta 단독(3.96×)보다 낫다.
- **hybrid mode 선택 개선(held-out)**: interleave IoT 가 1.73×→**2.04×** 로 올라 pos-delta(1.90×)를 넘었다
  (delta 자리 2→6). 다만 delta 가 압도적인 스트림(modbus 6.72>5.49, single-iot 6.45>4.79)에선 greedy
  per-position 선택이 아직 순수 pos-delta 에 못 미친다 — **pos-delta 가 천장**이고 hybrid 는 보수적.
  PR-2 인코더는 자리별로 더 나은 모드를 그대로 박으므로 천장(pos-delta)을 취할 수 있다.

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
- **PR-3**: 인코더에서 자리별 mode 를 천장(pos-delta) 수준으로 취하도록(greedy 선택을 pos-delta 우선/
  결합으로) 더 끌어올리기 + 실제 코딩 바이트 측정.

> mode 선택은 held-out 검증으로 이미 개선됨(interleave IoT 1.73→2.04×). 자리별 byte/delta 결정은
> `tools/bcb-structbench.c` 가 train 80/20 분할로 수행한다.
