# 실데이터 벤치마크 / Real-Data Benchmarks

> **목적:** README·`docs/benchmarks.md` 의 수치는 모두 `tests/scenarios/` 의 **합성
> generator** 출력 기준이다. B2B 고객은 자기 실데이터로의 재현을 요구한다. 이 문서는
> *실제* 공개 데이터에서 BCB 의 우위가 재현되는지 **정직하게** 검증한다.

## TL;DR (정직한 요약)

**합성 데이터에서 BCB 는 brotli 를 2배 이상 압도하지만, 실데이터에서는 동률이거나 진다.**

- 합성 구조형(binary_record, iot_single): BCB **5.4–6.3×** vs brotli ~2.6× → BCB 압승.
- 실제 IoT 양자화-정수 텔레메트리: BCB 3.67× ≈ brotli 3.69× → **무승부**.
- 실제 IoT float32 텔레메트리: BCB 3.47× vs brotli 3.99× → **BCB 패**.
- 실제 HTTP 응답 헤더(다양한 서버): BCB 2.53× vs brotli 3.75× → **BCB 완패**.

합성 generator 가 BCB 의 "자리별(position) 바이트/델타" 모델에 *딱 맞는* 규칙성을
만들어내기 때문이다. 실데이터에는 그 규칙성이 약하고, brotli/zstd 의 일반 LZ 가 잡는
교차-레코드 중복을 BCB 의 고정 자리 모델은 놓친다. **§7 전략적 함의 참고.**

---

## 1. 방법론 (Methodology)

- **도구:** `tools/bcb-realbench.c` (`make build/bcb-realbench`). 메시지 경계를 존중해
  실데이터를 압축하고, **모든 압축기에 동일한 train 구간을 공유 컨텍스트로** 준다.
- **비교 대상:** BCB(+landmark 또는 +structural) · brotli+dict(q11) · zstd+dict(maxlevel)
  · zlib+dict(raw deflate level 9, 32KB dict) · gzip(level 9, 무사전).
- **공정성:** 같은 메시지 집합, 모든 dict-가능 압축기에 같은 train. brotli/zstd 는 최대
  레벨. 측정은 각 압축기의 **코어 출력 바이트**(BCB=range-coder payload, 컨테이너/CRC 제외;
  brotli/zstd=코어 스트림; gzip/zlib=포맷 헤더 포함 — 큰 블록에선 무시 가능, 단일 소형
  레코드에선 불리하게 작용).
- **무손실:** 모든 메시지·모든 압축기에 대해 round-trip 을 검증한다(표의 `lossless` 열).
  하나라도 실패하면 도구가 비정상 종료(exit 3)한다. 본 문서의 모든 수치는 lossless=yes.

### 데이터셋 (출처·라이선스·재현)

| 데이터 | 출처 | 라이선스/재배포 | 취득 스크립트 |
|---|---|---|---|
| 실 HTTP/2 응답 헤더 | 84개 실 서버(위키·CDN·API·뉴스 등) 라이브 캡처 | 프로토콜 메타데이터(서버 응답). 실행마다 변함 → **데이터 미커밋** | `tests/corpus/fetch_http_real.sh` |
| 실 IoT 텔레메트리 | Intel Berkeley Lab sensor data (54 motes, ~2.3M readings, 2004) | 연구용 공개 데이터셋. **데이터 미커밋**(스크립트만) | `tests/corpus/fetch_iot_real.sh` |

데이터 자체는 재배포하지 않고(가이드라인 준수), **취득 스크립트만 커밋**한다. 코퍼스는
`build/` 아래 생성되며 gitignore 된다. 누구나 스크립트를 돌려 재현할 수 있다.

> **한계(정직):** HTTP 캡처는 *응답* 헤더(서버 출력)다. 진짜 브라우저 *요청* 스트림
> (한 커넥션의 HPACK 동적 테이블)은 브라우저 HAR 가 필요하며(`tests/scenarios/http2_real.py`),
> 본 측정의 "다양한 서버" 코퍼스는 BCB 가 *주장하는* 동질 스트림보다 **불리한** 조건이다.
> 그럼에도 우위가 재현되지 않는다는 사실은 그대로 기록한다.

---

## 2. 합성 vs 실데이터 — 나란히 (동일 도구·방법론)

### 구조형(structural, 자리별 모델) — 100 레코드/블록

| 데이터 (R=레코드크기) | BCB+struct | brotli+dict | zstd+dict | zlib+dict | gzip | BCB 블록 승률 |
|---|---|---|---|---|---|---|
| **합성** binary_record (R=32) | **5.410×** | 2.576× | 2.393× | 1.924× | 2.044× | 75/75 |
| **합성** iot_single (R=18) | **6.282×** | 2.566× | 2.066× | 1.641× | 1.758× | 80/80 |
| **실** IoT 양자화-정수 (R=10) | 3.670× | **3.693×** | 3.401× | 3.157× | 2.993× | 901/1500 |
| **실** IoT float32 (R=22) | 3.465× | **3.990×** | 3.947× | 3.353× | 3.115× | 832/1500 |

### 소형 메시지(landmark, text-like)

| 데이터 | BCB+lm | brotli+dict | zstd+dict | zlib+dict | gzip | BCB 메시지 승률 |
|---|---|---|---|---|---|---|
| **실** HTTP/2 응답 헤더 | 2.532× | **3.748×** | 3.605× | 3.484× | 1.915× | 1/42 |

(굵게 = 해당 행 최고 압축비.)

> **"BCB" 칼럼 구성:** 위 표의 BCB 는 **base(순수 BT)가 아니라 +landmark / +structural
> schema 가 켜진** 값이다 (HTTP=+landmark k=256, IoT=+structural). 모드를 끄지 않았다.

### 2.1 base(순수 BT) vs 강화 모드 — 모드 기여도 (실데이터, 동일 도구)

향상 모드가 실제로 켜져 있었음을 증명하고 기여도를 보이기 위해, 각 실데이터를 **순수
BT(base, `--landmark-k 0`)** 와 **강화 모드**로 각각 측정했다(brotli/zstd max, 전부 무손실):

| 실데이터 | BCB base(BT) | BCB +lm/+struct | brotli+dict | zstd+dict | zlib+dict | gzip |
|---|---|---|---|---|---|---|
| HTTP 응답 헤더 | 2.485× | **2.532×** (+landmark, +1.9%) | **3.748×** | 3.605× | 3.484× | 1.915× |
| IoT 양자화-정수 (R=10) | 1.544× | **3.670×** (+structural, +138%) | **3.693×** | 3.401× | 3.157× | 2.993× |
| IoT float32 (R=22) | 2.213× | **3.465×** (+structural, +57%) | **3.990×** | 3.947× | 3.353× | 3.115× |

→ 강화 모드는 분명히 켜져 있었고 **크게 기여**한다(structural 은 +57~138%). 그럼에도
**강화 모드를 켠 BCB 가** 양자화-정수에서 brotli 와 동률, float32·HTTP 에서는 진다.
즉 우위 미재현은 "모드를 안 켜서"가 아니다.

---

## 3. 배칭(batching)은 필수 — 단일 레코드는 아무도 못 줄인다

실 IoT 레코드를 **1개씩 독립** 압축하면 모든 압축기가 사실상 실패한다(소형 메시지 framing
오버헤드 + 단일 레코드 내 엔트로피가 높음):

| 데이터 (단일 레코드) | BCB | brotli+dict | zstd+dict | zlib+dict | gzip |
|---|---|---|---|---|---|
| 실 IoT float32 (R=22) | 0.998× | 0.870× | 0.753× | 1.234× | 0.519× |
| 실 IoT 양자화-정수 (R=10) | 0.987× | 0.716× | 0.526× | 0.967× | 0.333× |

→ 1.0× 미만 = **팽창**. 실 텔레메트리는 반드시 **블록(배치)**으로 압축해야 의미가 있다.
(§2 의 100 레코드/블록 결과가 실제 사용 형태.)

---

## 4. float32 vs 양자화-정수 — 인코딩이 결과를 가른다

같은 센서 값이라도 **전송 인코딩**이 압축성을 좌우한다:

- **float32**: IEEE-754 가수(mantissa) 하위 비트가 거의 난수 → 자리별 델타가 고엔트로피.
  BCB 의 구조형 우위가 사라지고 brotli/zstd 의 LZ 가 앞선다(3.47× vs 3.99×).
- **양자화-정수**(Modbus 레지스터·CAN 시그널처럼 스케일된 정수): 자리별 느린 드리프트가
  드러나 BCB 가 brotli 와 동률(3.67× ≈ 3.69×), zstd/zlib/gzip 보다는 우위.

실 IoT 프로토콜 다수가 정수 전송이라 양자화-정수가 더 대표적이지만, **그 경우에도 BCB 는
동률에 그친다**(합성에서의 압승과 대조).

---

## 5. 왜 합성에서는 이기고 실데이터에서는 못 이기나 (원인 분석)

1. **합성 generator 가 BCB 모델에 과적합되어 있다.** `binary_record.py`/`iot_packets.py`
   는 자리마다 카운터·고정 필드·작은 델타 같은 *규칙적* 패턴을 만든다. 이는 BCB 의 "자리별
   byte/delta 경험분포" 모델이 거의 완벽히 예측하는 구조다 → 5–6×, 블록 승률 100%.
2. **실데이터는 그 규칙성이 약하다.** 실 센서값은 지터·결측·비정상 타이밍·float 노이즈를
   포함한다. 자리별 모드가 깔끔히 안 맞고, 교차-레코드의 *비정렬* 중복이 많아진다.
3. **brotli/zstd 의 LZ 가 그 비정렬 중복을 잡는다.** BCB 의 고정 자리 모델은 레코드 경계에
   정렬된 통계만 보지만, LZ 는 임의 위치의 반복 부분열을 잡아 실데이터에서 앞선다.
4. **HTTP: 교차-메시지 공유 구조 부족.** 84개 *서로 다른* 서버의 응답 헤더는 메시지 간
   공유 컨텍스트가 약하다. BCB 의 landmark prior 는 반복되는 context 가 있어야 이득인데,
   이질적 실 헤더에는 그게 약하고 brotli+dict 의 사전 매칭이 앞선다.

---

## 6. 정리 — BCB 가 이기는/비기는/지는 구간

- ✅ **합성 구조형**: 압승(5–6×, 2배+). 단 이는 BCB 모델에 맞춘 데이터라는 점을 명시.
- 〰️ **실 양자화-정수 IoT(배치)**: brotli 와 동률, zstd/zlib/gzip 보다 우위.
- ❌ **실 float32 IoT(배치)**: brotli/zstd 에 13~14% 뒤짐.
- ❌ **실 HTTP 응답 헤더(다양 서버)**: brotli/zstd 에 크게 뒤짐(2.53× vs 3.75×).

---

## 7. 전략적 함의 (정직하게)

상업화 계획의 진행 메모는 **"실데이터에서 우위 미재현 시, 엔지니어링 투자를 멈추고 타깃
세그먼트를 재정의하라"**고 적시한다. 본 측정 결과는 **대체로 그 경고에 해당**한다:

- 현재 검증한 실데이터(HTTP 다양 서버, IoT float/int)에서 BCB 의 *결정적* 우위는
  재현되지 않았다. 최선의 경우가 "동률"(양자화-정수 배치)이다.
- 합성 벤치의 압도적 수치(5–6×)는 **generator 가 BCB 모델에 정렬되어** 나온 것으로,
  영업 자료로 쓰면 실데이터 재현 실패 시 신뢰를 잃을 위험이 크다.

**권고:**
1. 영업·README 수치를 "합성 generator 기준"으로 계속 명확히 표기(이미 일부 표기됨)하고,
   실데이터 수치(이 문서)를 함께 제시한다 — 과장 금지.
2. **타깃 세그먼트 재정의:** BCB 가 *실제로* 이길 가능성이 높은 구간을 찾는다 — 예:
   (a) 진짜 동질 스트림(한 클라이언트↔한 API 의 반복 요청 헤더; HPACK 동적테이블 대체),
   (b) 정수 고정 레코드 중 자리별 델타가 작은 고정밀 프로토콜, (c) prior 를 충분히 크고
   대표성 있게 학습한 경우. 이들에 대한 *실데이터* 재현을 추가 검증해야 한다.
3. 위 (a)~(c) 중 하나라도 실데이터에서 brotli 대비 명확한(예: ≥20%) 우위가 재현되기
   전에는, CMake/바인딩/릴리스 자동화 같은 후속 엔지니어링 투자를 보류하는 것을 고려.

> 이 결과는 BCB 가 "쓸모없다"는 뜻이 아니다. 양자화-정수 배치에서 brotli 와 동률이면서
> libm-free·임베디드·작은 코드라는 *운영 특성*은 여전히 가치가 있을 수 있다. 다만
> **"압축비로 LZ 를 압도한다"는 합성 기반 주장은 실데이터로 뒷받침되지 않는다.**

---

## 8. 재현 (Reproduction)

```sh
# 1) 도구 빌드 (brotli/zstd/zlib dev 필요)
make build/bcb-realbench

# 2) 실데이터 취득 (build/ 아래 생성, gitignore)
sh tests/corpus/fetch_http_real.sh        # build/real_http.corpus (+ .index)
sh tests/corpus/fetch_iot_real.sh         # build/real_iot.corpus, build/real_iot_int.corpus

# 3) 측정 (모든 압축기 round-trip 무손실 검증)
./build/bcb-realbench --corpus build/real_http.corpus --index build/real_http.index \
    --train-msgs 42 --mode landmark --landmark-k 256 --label "real HTTP"
./build/bcb-realbench --corpus build/real_iot_int.corpus --record-size 10 --recs-per-msg 100 \
    --train-msgs 1000 --max-test 1500 --mode structural --label "real IoT int"
./build/bcb-realbench --corpus build/real_iot.corpus --record-size 22 --recs-per-msg 100 \
    --train-msgs 1000 --max-test 1500 --mode structural --label "real IoT float32"

# 합성 대조 (동일 도구)
make build/corpus_binary_record.bin build/corpus_iot_single.bin
./build/bcb-realbench --corpus build/corpus_binary_record.bin --record-size 32 --recs-per-msg 100 \
    --train-msgs 50 --max-test 80 --mode structural --label "SYNTH binary_record"
```

### 한계·주의 (재차)

- HTTP 코퍼스는 라이브 캡처라 실행마다 다르고 작다(84 블록). train 도 작다 → BCB 에 다소
  불리할 수 있으나, 코퍼스가 작은 것은 "다양 서버 실헤더"의 특성이기도 하다.
- IoT 레코드는 per-device(moteid, epoch) 정렬한 단일 디바이스 스트림이다. 인터리브된
  멀티 디바이스 스트림은 모든 압축기에 더 어렵다.
- brotli/zstd 는 최대 레벨(q11/lvl22)로, 속도가 아닌 **압축비 상한**을 비교한 것이다.
  BCB 의 강점 중 하나인 *속도/메모리/임베디드 적합성*은 본 문서가 다루지 않는다.
