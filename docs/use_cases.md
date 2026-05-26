# BCB Use Cases — 어디에 쓰나 / Where BCB fits

BCB 의 강점은 단 하나의 조건에서 나온다: **작은 메시지 + 인코더·디코더가 공유하는 학습 prior**.
이 조건이 맞는 4가지 대표 시나리오와, 맞지 않는 경우를 정리한다.

전제 (모든 시나리오 공통):
- 인코더와 디코더가 **같은 도메인 코퍼스로 학습한 동일 prior** 를 가진다 (공간은 전송 안 함).
- 메시지가 작다 (≈10–512B). 작을수록 BCB 우위가 커진다 (`docs/benchmarks.md`).
- 무손실이 필요하다.

---

## 1. IoT 텔레메트리 패킷 (10–500B)

센서 노드 ↔ 게이트웨이가 같은 패킷 스키마 prior 를 공유한다. 18B 패킷 같은 작은
바이너리에서 brotli/zstd 는 프레임 overhead 때문에 **데이터를 오히려 키우는**(64B 에서 0.95×)
반면, BCB 는 1.5× 압축을 유지한다 (`iot_packets` 측정).

- 적합: LoRaWAN / NB-IoT 처럼 1패킷당 바이트가 곧 전력·요금인 환경.
- prior 공유: 펌웨어에 학습된 prior 를 베이크 (MCU 빌드 3.56MB, `docs/mcu.md`).

## 2. HTTP/2 헤더 압축 대안 (HPACK 후보)

요청 헤더 블록(수십~수백 B)을 공유 prior 로 압축. 64B 구간에서 BCB 가 brotli+dict 를
+22% 앞선다 (`http_headers`). 다만 텍스트 헤더는 LZ 친화적이라 128B 이상에서는 brotli 가
앞서므로, **콜드 스타트/소형 헤더**에 한정한 대안으로 본다. HPACK 직접 비교는 후속 과제(#4).

## 3. MQTT / CoAP 발행 메시지

토픽 구조가 고정적이고 페이로드가 작은 pub/sub. `mqtt_messages` 측정에서 BCB 가
**512B 까지** brotli+dict·zstd+dict 를 모두 앞선다 (4.0–4.4×) — 5개 시나리오 중 가장 넓은
우위 구간. 브로커·클라이언트가 토픽/페이로드 스키마 prior 를 공유하는 구조에 적합.

## 4. 임베디드 로그 라인 / RPC 호출

- **syslog 스타일 로그**(`log_lines`): 타임스탬프·호스트·레벨 등 구조적 중복이 커서
  256B 까지 BCB 우위. 디바이스가 로그를 적게 올려야 하는 망에 적합.
- **gRPC-like 소형 호출**(`rpc_calls`): 메서드 path + 작은 protobuf 바디. 256B 까지 BCB 우위.
- 그 외: 푸시 알림, 짧은 RPC 응답 등 "구조가 반복되는 작은 메시지" 전반.

---

## BCB 가 맞지 않는 경우 / When NOT to use BCB

- **≳1–2KB 데이터**: brotli/zstd 의 LZ77 long-range matching 이 이긴다. 그쪽을 써라
  (더 빠르고 더 잘 압축). 교차점은 대략 512B~1KB (`docs/benchmarks.md`).
- **prior 를 공유할 수 없을 때**: BCB 의 모든 이점은 양쪽이 같은 prior 를 외우는 데서 나온다.
  일회성·임의 데이터에는 부적합.
- **랜덤/이미 압축된 데이터**: 압축 불가 (Shannon). 정상.
- **LZ 친화적 큰 텍스트**: 반복 토큰이 많은 긴 텍스트(예: 4KB 책 발췌)는 brotli 가 앞선다
  (`docs/benchmarks_legacy.md`).

## 한 줄 결론

BCB 는 **"양쪽이 같은 prior 를 공유하는 작은 메시지(≤256B)"** 라는 좁지만 실재하는 틈에서
LZ+dict 를 이긴다. 그 밖에서는 brotli/zstd 를 써라.
