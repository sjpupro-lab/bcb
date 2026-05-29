# 실데이터 벤치마크 / Real-Data Benchmarks

> **목적:** README·`docs/benchmarks.md` 수치는 모두 `tests/scenarios/` 의 **합성
> generator** 출력 기준이다. B2B 고객은 자기 실데이터로의 재현을 요구한다. 이 문서는
> *실제* 공개 데이터에서, **현장 전송 방식 그대로(엣지=패킷 단위 개별 압축)** 우위가
> 재현되는지 정직하게 검증한다.

> **⚠️ 측정 설계 정정 이력:** 초판은 IoT 를 100 레코드/블록(1–2.2KB)으로만 재 "우위 미재현"
> 이라 결론냈다. 이는 두 가지가 틀렸다. (1) 1–2KB 는 BCB 가 *스스로* 약세라 밝힌 구간이고,
> (2) **현장의 IoT/MCU 는 메시지를 모아 압축하지 않는다** — 센서가 패킷 하나를 만들면
> 즉시 개별 전송한다(메모리·전력·실시간 제약). 아래는 **패킷 단위(per-message) 개별 압축**
> 으로 다시 설계한 정정본이며, "엣지(per-message)" vs "게이트웨이(batch)" 두 시나리오를
> 분리한다.

---

## 0. 가장 중요한 전제 — 타깃 환경에서 brotli/zstd 는 "동작하지 않는다"

엣지 IoT/MCU 가 BCB 의 정체성이자 타깃이다. 이 환경에서 brotli/zstd 와의 *압축비* 비교는
**대부분 무의미**하다. 이유:

- 본 문서의 brotli/zstd 수치는 **최대 설정**(brotli quality 11 / LGWIN 24, zstd level 22)
  이다. 이 설정의 인코더 작업 메모리는 **MB 단위**(brotli 대형 윈도, zstd 최대 레벨 매치
  테이블)로, 소형 MCU SRAM(예: RP2040 264KB)을 **초과**한다. 즉 **그 압축비는 디바이스에서
  애초에 달성 불가**다. MCU 에 맞는 저설정으로 낮추면 압축비가 급락한다.
- BCB 는 설계상 이 환경을 위한 것이다: **libm-free·정수 전용 hot path**, prior 는 **frozen
  읽기전용**(flash/PSRAM 상주), 인코더 작업 상태는 소형. MCU 빌드(`-DBCB_MCU`) 총
  3.56MB(학습 테이블 포함), 디바이스 인코드 경로는 더 작다(`docs/mcu.md`). ESP32/RP2040 동작.

> **따라서 엣지 시나리오에서 올바른 비교는 "BCB vs brotli 압축비"가 아니라 "BCB vs 무압축
> (또는 그 환경에서 실제로 돌릴 수 있는 약한 압축)"이다.** 아래 엣지 표의 brotli/zstd 열은
> *참고용*(대형 CPU/게이트웨이 가정)일 뿐, 타깃 MCU 에 배포 가능하지 않다(✗ 표기).
> brotli/zstd 가 정당한 경쟁자가 되는 곳은 **게이트웨이(§3)** — 리눅스 박스 — 뿐이다.

---

## 1. TL;DR

- **엣지(패킷 단위, MCU): BCB 가 배포 가능한 유일한 강압축기**다. 현실적 패킷 크기
  (수십~수백 B)에서 BCB **1.8–2.7×**, 같은 설정의 brotli 는 ~1× 안팎(거의 무효)이며
  애초에 MCU 에 안 올라간다. → **타깃 적중, 실데이터로 재현됨.**
- **게이트웨이(배치, 리눅스): BCB ≈ brotli**(정수 1000B: 3.80× vs 3.73×, float 2.2KB:
  3.56× vs 4.02×). 큰 배치에선 brotli 가 따라잡는다 — BCB 문서의 주장과 일치.

---

## 2. 엣지 노드 — per-message (각 패킷을 개별 압축)

**배포 환경:** 센서/MCU 가 패킷을 만들 때마다 즉시 개별 전송(LoRa/NB-IoT/BLE/Zigbee 등).
배치 없음. 각 패킷을 독립적으로 압축. **모든 압축기가 동일한 단일 패킷 단위로 측정**되며,
패킷마다 인코더를 새로 초기화한다(메시지 이어붙이기 없음). 전부 round-trip 무손실.

### 실 IoT 양자화-정수 (R=10; Modbus/CAN 류 스케일된 정수)

| 패킷 크기 | **BCB+struct (배포가능)** | brotli+dict ✗MCU | zstd+dict ✗MCU | gzip ✗MCU |
|---|---|---|---|---|
| 10 B (1 reading) | **0.923×** | 0.715× | 0.526× | 0.333× |
| 20 B (2) | **1.601×** | 0.837× | 0.695× | 0.542× |
| 40 B (4) | **2.614×** | 0.975× | 0.925× | 0.877× |
| 80 B (8) | **2.134×** | 1.294× | 1.292× | 1.258× |

### 실 IoT float32 (R=22; 원시 IEEE-754 전송)

| 패킷 크기 | **BCB+struct (배포가능)** | brotli+dict ✗MCU | zstd+dict ✗MCU | gzip ✗MCU |
|---|---|---|---|---|
| 22 B (1 reading) | 0.848× | 0.864× | 0.738× | 0.520× |
| 44 B (2) | **1.447×** | 0.977× | 1.077× | 0.814× |
| 88 B (4) | **2.250×** | 1.233× | 1.520× | 1.193× |
| 176 B (8) | **2.223×** | 1.624× | 1.882× | 1.565× |

**관찰:**
- **2개 reading 이상(20–80B 정수, 44–176B float)에서 BCB 가 압도적**이다. 같은 패킷에서
  brotli/zstd 는 ~1×(거의 무압축)인데 BCB 는 1.4–2.6×. 게다가 그 brotli 수치는 *최대
  설정* 가정이라 MCU 엔 올라가지도 않는다 — 디바이스 실비교는 **BCB vs 무압축**.
- **단일 reading(10–22B)**은 모두 손익분기 미만(너무 작음). 현실 패킷은 보통 여러 필드/
  reading 을 담으므로 20B+ 구간이 대표적이다.
- ✗MCU = 해당 압축비를 내는 설정이 타깃 MCU SRAM 을 초과 → 디바이스 배포 불가(§0).

---

## 3. 게이트웨이 — batch (여러 reading 을 모아 한 페이로드로)

**배포 환경:** 엣지에서 올라온 텔레메트리를 **리눅스 게이트웨이/서버**가 모아 저장·전송 전
배치 압축. 여기선 RAM·libm 제약이 없어 brotli/zstd 가 **정당한 경쟁자**다.

| 배치(=100 reading) | BCB+struct | brotli+dict | zstd+dict | gzip |
|---|---|---|---|---|
| 정수 1000 B | **3.798×** | 3.729× | 3.434× | 3.023× |
| float 2200 B | 3.560× | **4.017×** | 3.980× | 3.133× |

→ 게이트웨이 배치에선 **BCB ≈ brotli**(정수 근소 우세, float 근소 열세). 큰 배치는
brotli 의 LZ 가 따라잡는 구간으로, BCB 가 README 에서 밝힌 한계와 일치. 즉 게이트웨이
티어에서는 BCB 의 압축비 이점이 거의 없다(운영 특성으로 차별화해야).

---

## 4. 합성 vs 실데이터 (동일 도구·동일 단위, 정직 비교)

| 패킷/단위 | 합성 binrec(R=32) BCB / brotli | 실 정수(R=10) BCB / brotli |
|---|---|---|
| 1 reading | 1.24× / 0.89× | 0.92× / 0.72× |
| 작은 패킷(4–8 rec) | 3.66× / 1.81× | 2.1–2.6× / 0.98–1.29× |
| 배치(100 rec) | 5.41× / 2.58× | 3.80× / 3.73× |

- **작은 패킷에서 BCB 가 brotli 를 크게 앞서는 패턴은 합성·실 양쪽에서 동일**(엣지 타깃 확증).
- 차이는 **배치(큰 블록)** 에서다: 합성은 BCB 우위가 계속 벌어지지만(5.4× vs 2.6×), 실데이터
  는 brotli 가 따라잡아 동률. → **합성의 큰-블록 수치는 과장**이며 영업에서 빼거나 "합성
  한정"으로 강하게 한정해야 한다. 반면 **엣지(작은 패킷) 우위는 실데이터에서 유지**된다.

---

## 5. 정직한 반례 — HTTP 응답 헤더(다양 서버)

| 실 HTTP/2 응답 헤더 (per-block) | BCB+lm | brotli+dict | zstd+dict | gzip |
|---|---|---|---|---|
| 헤더 블록 1개(평균 ~1.3KB) | 2.532× | **3.748×** | 3.605× | 1.915× |

BCB 가 진다. (1) 메시지가 ~1.3KB 로 이미 BCB 약세 구간, (2) 84개 *서로 다른* 서버라 공유
구조가 약해 landmark 기여 미미(base 2.485 → +lm 2.532). landmark/structural 은 켜져 있었다.
진짜 HPACK 사용처(한 클라이언트↔한 서버 반복 요청, 동질 스트림)는 브라우저 HAR 가 필요해
별도 검증 대상(`tests/scenarios/http2_real.py`). **현 측정은 BCB 타깃이 아닌 구간.**

---

## 6. 방법론 (Methodology)

- **도구:** `tools/bcb-realbench.c` (`make build/bcb-realbench`). **메시지(패킷) 단위로** 압축,
  모든 압축기에 **동일한 단일 패킷 `(m, ml)`** 과 동일 train(공유 prior/dict)을 준다.
  메시지를 이어붙여 한 번에 압축하지 **않는다**(코드 루프가 같은 m,ml 을 각 압축기에 개별 전달).
- **패킷 단위:** IoT=`--record-size R`·`--recs-per-msg K`(K=패킷당 reading 수, 패킷마다 독립
  압축). HTTP=헤더 블록 1개(`--index`). 합성·실데이터 **같은 단위**.
- **비교:** BCB(+landmark/+structural) · brotli+dict(q11) · zstd+dict(maxlvl) · gzip(l9 무사전)
  · zlib+dict(생략 가능). brotli/zstd 최대 레벨(압축비 상한). 측정은 코어 출력 바이트.
- **무손실:** 패킷·압축기마다 round-trip 검증(표 전부 lossless=yes). 실패 시 도구 비정상 종료.

### 데이터셋

| 데이터 | 출처 | 라이선스 | 취득 |
|---|---|---|---|
| 실 HTTP/2 응답 헤더 | 84개 실 서버 라이브 캡처 | 프로토콜 메타데이터(미커밋) | `tests/corpus/fetch_http_real.sh` |
| 실 IoT 텔레메트리 | Intel Berkeley Lab sensor data (2004) | 연구용 공개(미커밋, 스크립트만) | `tests/corpus/fetch_iot_real.sh` |

데이터는 재배포 안 함(스크립트만 커밋). 코퍼스는 `build/`(gitignore) 생성.

---

## 7. 전략적 함의

1. **타깃 = 엣지 IoT/MCU 의 작은 패킷(per-message).** 여기서 BCB 는 실데이터로 1.8–2.7×
   를 재현하며, **유일하게 배포 가능한 강압축기**다(brotli/zstd 는 그 압축비 설정으로 디바이스
   에 못 올라감). 영업 메시지는 "압축비 X배"보다 **"MCU 에서 실제로 돌아가는 무손실 압축"**
   에 무게를 둔다 — 압축비는 그 위의 보너스.
2. **게이트웨이/서버 티어는 BCB 의 영역이 아니다.** 거기선 brotli ≈ BCB 이고 brotli 가 자유
   롭게 돈다. 게이트웨이를 타깃으로 팔지 말 것.
3. **합성 큰-블록 수치(5–6×)는 과장.** 실데이터 배치는 brotli 와 동률. README 큰-블록 합성
   수치는 "합성 한정" 강조 또는 영업 제외.
4. **후속 엔지니어링(CMake·바인딩·릴리스)은 진행 가치가 있다** — 단 "엣지 MCU 의 작은
   메시지 무손실 압축"으로 포지셔닝하고, MCU 풋프린트·전력·지연의 실측을 영업 자료의
   1순위 근거로 준비할 것(압축비 단독이 아니라).

> 초판의 "투자 중단" 권고를 **철회**한다. 부정적 결론은 측정 단위를 현장과 무관한 큰 배치
> 로 잡은 설계 오류 때문이었다. 현장 방식(엣지 per-message)으로 재면 **실데이터에서 우위가
> 재현**되고, 더 중요하게 **그 환경에서 BCB 는 경쟁자가 아예 없다**(brotli/zstd 미배포).

---

## 8. 재현 (Reproduction)

```sh
make build/bcb-realbench
sh tests/corpus/fetch_iot_real.sh    # build/real_iot.corpus, build/real_iot_int.corpus

# 엣지: 패킷 단위 개별 압축 (K = 패킷당 reading 수). 모든 압축기 동일 단위·무손실.
for k in 1 2 4 8; do
  ./build/bcb-realbench --corpus build/real_iot_int.corpus --record-size 10 \
    --recs-per-msg $k --train-msgs 1000 --max-test 3000 --mode structural --label "edge int K=$k"
done

# 게이트웨이: 배치 압축
./build/bcb-realbench --corpus build/real_iot_int.corpus --record-size 10 \
    --recs-per-msg 100 --train-msgs 1000 --max-test 3000 --mode structural --label "gateway int batch"

# MCU 풋프린트(왜 brotli/zstd 가 엣지에 못 올라가는지)
make meminfo
```

### 한계·주의

- 엣지의 "✗MCU" 는 *해당 압축비를 내는 최대 설정*이 MCU SRAM 을 초과한다는 의미다. brotli/
  zstd 를 저설정으로 빌드해 MCU 에 올릴 수는 있으나, 그 경우 압축비는 표보다 훨씬 낮다.
  본 표는 그들에게 *유리한* 최대 설정을 줬는데도 작은 패킷에서 BCB 에 진다는 점을 보인다.
- 단일 reading(10–22B)은 너무 작아 모두 손익분기 미만. 현실 패킷은 보통 20B+.
- IoT 레코드는 per-device(moteid,epoch) 정렬 단일 스트림. brotli/zstd 는 압축비 상한 비교이며
  BCB 의 속도/메모리/임베디드 적합성은 별도 강점.
