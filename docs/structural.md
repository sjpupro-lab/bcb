# Structural Codec (v6 PR-2) — entropy 이론 vs 실측 byte

`src/v5_mmap_prior/` 의 record schema 로 위치(position) 기반 압축을 **실제 코덱**에 통합했다.
측정 도구(`make structbench`, entropy 추정)를 **실제 코딩 바이트 + round-trip 무손실**로 전환.
재현: `make structural-bench`. 빌드: `bcb-prior-build train out --schema-record-size R`.

## 설계 (무손실 구조적 보장)

- prior 파일에 record schema section: `record_size`, 자리별 `mode`(byte/delta), 자리별 정수
  양자화 cum(256 width, 합=range scale, 각 ≥1).
- 인코더/디코더: 자리 p 에서 schema cum 사용. **delta 모드**는 cum 을 `prev_record[p]` 만큼
  **순환 시프트** — 실제 byte 를 그대로 코딩하되 분포만 직전 레코드 기준 정렬. enc/dec 가 동일
  정수 cum + 동일 prev → **런타임 부동소수 없음 → 무손실 구조적 보장**.
- schema 빌드는 오프라인(float): 자리별 byte/delta 분포 + held-out(80/20) mode 결정 → 정수 양자화.
- schema 없는 prior(record_size=0)는 기존 동작 그대로(landmark/BT) — **backward compat**.

## 실측 — `make structural-bench`

train ~50KB(record-aligned), message = 100 records, **실제 압축 byte + round-trip 무손실**:

| 시나리오 | rec | base× | **struct×** | 이득 | lossless | entropy(hybrid) 대비 |
|---|---|---|---|---|---|---|
| binary_record | 32 | 1.39 | **5.35** | +285.6% | yes | 5.49 (−3%) |
| iot (single device) | 18 | 1.28 | **3.99** | +211.3% | yes | 4.79 (−17%) |
| modbus | 25 | 1.44 | **3.67** | +154.4% | yes | 5.49 (−33%) |
| canbus | 16 | 2.49 | **3.91** | +57.3% | yes | 4.36 (−10%) |
| iot (8-dev interleave) | 18 | 1.30 | **1.83** | +41.2% | yes | 2.04 (−10%) |

비교: gzip/zstd/brotli 는 이 작은 binary 레코드에서 ~0.95×(오히려 키움).

## 정직한 해석 — 이론(entropy) vs 실측(byte)

- **전부 무손실**, 전부 ≥1.8× 목표를 큰 폭으로 상회(텍스트형이 아닌 binary 에서 BCB 가 진짜 압축).
- **binary_record 는 이론치와 거의 일치(−3%)** → 코덱 자체는 건전하다.
- 나머지 gap(−10~−33%)의 원인:
  1. **양자화** — 14-bit range scale + 256-심볼 min-1 floor. 거의 결정적인 자리도 심볼당 하한
     ~0.023 bit 를 문다.
  2. **per-message framing** — delta 는 직전 레코드가 필요하다. 메시지 첫 레코드(prev=0)는 손해라
     메시지가 짧을수록 페널티↑. message=1 record 면 delta 무효(pos-byte 만). 스트림이 길수록 이론치에 근접.
  3. **greedy hybrid 보수성** — 자리별 mode 를 held-out 으로 고르나 일부 delta-우세 자리를 byte 로
     골라 손해(modbus 가 −33% 로 가장 큼: 레지스터 자리 일부가 byte 선택). pos-delta 강제가 천장.
  4. entropy 추정은 이상적 하한(완전 정밀 + 연속 스트림 가정)이다.
- 즉 "±10% 이내" 목표는 binary_record(−3%)·iot_packets(−10%)·canbus(−10%)에서 성립, iot_single
  (−17%)·modbus(−33%)는 위 요인으로 미달. **실측 도약 자체는 확정**(base 대비 +41~286%, 무손실).

## records-per-message 곡선 (single-device IoT, structural)

delta 는 스트림이 길수록 효과 — 첫 레코드 페널티가 amortize 된다:

| message | records | struct× |
|---|---|---|
| 18B | 1 | 0.91 (delta 무효 + framing) |
| 144B | 8 | 3.03 |
| 576B | 32 | 3.74 |
| 1800B | 100 | 3.99 |
| 9000B | 500 | 4.12 |

**결론**: structural codec 은 per-device/streaming binary 레코드에서 무손실로 base 대비 큰 도약을
실측 제공한다. 추가 개선 여지(양자화 정밀도↑, mode 선택 강화)는 후속.
