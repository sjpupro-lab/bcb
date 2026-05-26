# Landmark Prior Index — 수집 + coverage 측정 (PR-1)

prior 안의 빈출 context 를 "이정표(landmark)"로 뽑아, 인코더·디코더가 양쪽 동일하게 보유한다.
재현: `make landmark` (N=8) / `make landmark LM_N=16`. 도구: `tools/bcb-landmark.c`.

## 두 메커니즘 — 측정으로 구분

landmark hit 시 미리 계산한 분포(cum)를 써서 `bt_v3_distribution` 호출을 건너뛴다. 그 cum 이
무엇이냐에 따라 효과가 갈린다:

- **(a) 캐싱** — BT 가 내놓는 *그* 분포를 저장·재사용. **비트 동일 → 압축비 0% 변화**, 속도만 ↑.
- **(b) sharper 모델** — depth-N 경험적 분포(+backoff)를 저장. 빈출 context 에선 BT 의 6-레벨
  blend 보다 날카로워 **압축비가 변할 수 있다**(보통 향상). 디코더가 같은 표를 가지면 무손실.

둘 다 hit 시 predict 를 건너뛰므로 **속도 이득은 같다**. (b)는 추가로 압축비를 올리므로
**(b)가 (a)를 지배한다** → PR-2 는 sharper cum 을 저장한다. (원 기획서의 "cached cum + +48% ratio"
는 (a)와 (b)를 섞은 표현이다: 캐싱만으로는 ratio 가 오르지 않는다.)

아래 표의 `land ratio`/`gain%` 는 (b) 기준 **entropy 추정치**다 (실제 코딩 바이트·무손실은 PR-2/3).
backoff: `P = emp/(total+W) + W·P_BT/(total+W)`, W=64.

## Coverage (hit rate) — N=8

`make landmark` 실측 (train 50KB, test 50KB). hit% = N-byte context 가 top-K 에 드는 위치 비율.

| 시나리오 | K=8 | K=32 | K=128 | K=512 | K=2048 | K=4096 |
|---|---|---|---|---|---|---|
| http_headers | 3.9 | 12.5 | 38.6 | 73.2 | 92.1 | 92.6 |
| mqtt_messages | 8.9 | 19.1 | 33.7 | 49.0 | 67.5 | 75.6 |
| rpc_calls | 5.1 | 15.1 | 40.0 | 61.7 | 63.7 | 64.0 |
| log_lines (syslog) | 2.0 | 5.7 | 19.0 | 40.7 | 52.7 | 54.8 |
| iot_packets | **0.0** | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

## 압축비 잠재력 (gain%, base→land ratio) — N=8

gain% = (base bits − landmark bits) / base bits. base = 현재 BT blend 전 구간.

| 시나리오 | base ratio | K=128 | K=512 | K=2048 | K=4096 (land ratio) |
|---|---|---|---|---|---|
| http_headers | 5.65× | +19.6% | +33.6% | +38.6% | **+38.6% (9.20×)** |
| rpc_calls | 3.81× | +11.6% | +16.8% | +16.8% | +16.8% (4.58×) |
| mqtt_messages | 4.54× | +11.0% | +13.8% | +15.2% | +15.4% (5.36×) |
| log_lines | 3.39× | +5.7% | +10.3% | +11.0% | +11.0% (3.81×) |
| iot_packets | 1.36× | 0% | 0% | 0% | 0% (1.36×) |

## N=8 vs N=16

N=8 이 coverage·gain 모두 우세 (짧은 context 가 더 자주 반복). 예: mqtt K=4096 gain
N=8 +15.4% vs N=16 +2.7%; http +38.6% vs +32.6%. **PR-2 기본값 N=8.**

## 정직한 결론

- **landmark 는 텍스트형 반복 데이터에서 강력**: http coverage 92%, gain +38.6%(원 기획 +48% 에
  근접). mqtt/rpc/syslog 는 +11~17%.
- **binary 고엔트로피(IoT)에는 무효**: 18B 패킷의 timestamp/seq/센서값이 매번 달라 8-byte
  context 가 반복되지 않는다(coverage 0%, gain 0%). landmark 는 데이터 의존적 — 만능이 아니다.
- 원 기획서 hit rate(8→43.1%, 4096→56.5%)·ratio(+48%)는 **단일 시나리오로 재현되지 않는다.**
  방향(K↑→coverage↑, sharper→gain)은 맞고, 절대값은 시나리오 의존이며 IoT 는 0 이다.
- 압축비 이득은 **(b) sharper 모델**에서만 나온다. 캐싱(a)은 속도만.

## PR-2 — 저장 + 인코더 통합 (실측 검증)

prior 파일에 landmark section 추가: top-K 길이-N context + **정수 양자화된 sharper cum**
(byte width 256개, 합=CEC_RC_SCALE, 각 ≥1). 인코더/디코더는 hit 시 *저장된 동일 정수 width* 를
그대로 쓴다 — 런타임 재계산이 없으므로 부동소수 발산이 원천 불가, **무손실은 구조적으로 보장**.
빌드 시에만 (오프라인) 경험적+BT-blend backoff 를 float 로 계산해 정수로 양자화한다.

- 빌드: `bcb-prior-build train out --landmark-n 8 --landmark-k 512`
- 사용: `bcb-cli encode/decode --prior out` (landmark 있으면 자동 사용). 검증: `make landmark-verify`.

`make landmark-verify` 실측 (train 50KB, 2000×128B 블록, K=512, N=8) — **실제 코딩 바이트**:

| 시나리오 | base 압축비 | landmark 압축비 | 이득 | 속도(base→lm blocks/s) | lossless |
|---|---|---|---|---|---|
| http_headers | 5.52× | **7.50×** | +35.8% | 402 → 1569 (3.9×) | yes |
| mqtt_messages | 4.24× | **4.87×** | +14.8% | 511 → 1081 (2.1×) | yes |
| log_lines | 3.27× | **3.56×** | +9.0% | 554 → 1043 (1.9×) | yes |
| iot_packets | 1.296× | 1.296× | +0.0% | 857 → 846 (~동일) | yes |

- **압축비 이득이 PR-1 entropy 추정과 일치**(http +35.8% vs 추정 +33.6%, mqtt +14.8% vs +13.8% 등).
- **속도도 1.9~3.9× 향상** — hit 시 `bt_v3_distribution` 을 건너뛰고 저장 cum 사용.
- **IoT 는 ratio·속도 변화 없음**(coverage 0%; 미스 lookup overhead ~1.3% 뿐) — 무손실 유지.
- 4 시나리오 전부 무손실, 압축비 회귀 없음(향상 또는 동일).

메모리: landmark 가 prior 파일에 `K·N + K·512` 바이트 추가(K=512,N=8 → ~264KB), 로드 시 RAM
해시(K·2 int) 만. 비-landmark prior(lm_k=0)는 기존과 비트 동일 동작.

## PR-3 계획

throughput(메시지/초)·cold-start 시간·random-access 시연 + msgbench 표/README 갱신.
