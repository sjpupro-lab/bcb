# BCB vs HPACK — HTTP/2 헤더 압축 비교

`tools/bcb_vs_hpack.py` 실측. 재현: `make hpack` (또는 실제 트래픽: `http2_real.py capture.har`).

## 방법

- 같은 헤더 블록 시퀀스에서 BCB 와 HPACK(RFC 7541, pip `hpack`)을 비교한다.
- **원본 크기** = 텍스트 직렬화(`name: value\r\n`…) 바이트 — 두 압축기 ratio 의 공통 기준.
- **BCB**: 학습 이력(history)으로 만든 frozen `.bcb-prior`. 각 블록을 **독립**(stateless) 압축.
- **HPACK** 2 regime:
  - **cold** — 블록마다 새 encoder (연결 첫 요청; static table + Huffman, 동적 테이블 이점 없음).
  - **warm** — 방향별 encoder 를 학습 이력으로 미리 채운 뒤 test 블록을 **stateful** 압축
    (동적 테이블 populated — 양쪽이 같은 과거 트래픽을 본 가장 공정한 비교).

학습 이력 175 블록(50031 B), test 2825 블록.

## 결과

| category | n | avg(B) | BCB(B) | BCB(×) | HPACK-cold(B) | cold(×) | HPACK-warm(B) | warm(×) |
|---|---|---|---|---|---|---|---|---|
| all | 2825 | 295.5 | 50.3 | 5.87 | 148.7 | 1.99 | 44.9 | 6.58 |
| request | 1383 | 398.6 | 59.2 | 6.73 | 211.9 | 1.88 | 35.9 | 11.12 |
| response | 1442 | 196.6 | 41.8 | 4.70 | 88.2 | 2.23 | 53.6 | 3.67 |

(B=블록당 평균 압축 바이트, ×=원본/압축. BCB summary: blocks=2825 lossless=yes total_orig=834687 total_comp=142129 ratio=5.873 enc_ms_per_block=5.868 blocks_per_s=170)

처리 시간(wall, 참고): BCB 74.745s(프로세스 포함, C), HPACK-cold 0.172s, HPACK-warm 0.119s (Python).
Python HPACK 과 C BCB 의 시간은 언어가 달라 직접 비교 불가 — 압축비가 핵심 지표다.

## 해석 (정직하게)

- **cold-start**: 동적 테이블이 비어 HPACK 은 static table(61개) + Huffman 만 쓴다. 학습 prior 를
  가진 BCB 가 이 구간에서 BCB 우세. 연결 첫 요청·짧은 연결(HTTP/3 0-RTT, 모바일 재연결)에서 의미.
- **warm (steady state)**: HPACK 의 동적 테이블이 반복 헤더를 인덱스 1~2 바이트로 줄인다.
  stateless BCB 는 매 블록 prior 비용을 다시 치르므로 이 구간은 전체적으로 HPACK 우세.
- **response 헤더**: warm 에서도 BCB(4.70×)가 HPACK-warm(3.67×)을 앞선다 — 응답 헤더는 date/content-length/etag 등 가변 필드가 많아 동적 테이블 인덱싱이 약하고, BCB 의 prior 가 이를 더 잘 다룬다.
- 즉 BCB 의 기회는 **stateful 동적 테이블을 유지할 수 없는/원치 않는 상황**(연결당 1~수 요청,
  무상태 게이트웨이, 동적 테이블 메모리/HOL-blocking 회피)이다. 장수 연결의 반복 request 트래픽은 HPACK 영역.
- **비용**: BCB 는 블록당 range-coder 연산이 무거워 HPACK 보다 CPU 가 많이 든다(위 blocks/s 참고).
  압축비 이득은 처리량과 trade-off 다 — 헤더가 작고 연결이 짧을수록 BCB 가 합리적.

## HTTP/3 (QPACK) 후보성

QPACK 은 HOL-blocking 때문에 동적 테이블 사용을 보수적으로 제한한다(많은 구현이 static-only
또는 제한적 dynamic). **동적 테이블을 끄거나 줄인 QPACK ≈ HPACK-cold** 에 가깝고, 그 구간에서
BCB 가 우세하면 "공유 prior 기반 무상태 헤더 압축"은 검토할 가치가 있다. 단 (1) 양측이 prior 를
배포·동기화해야 하고, (2) warm 반복 트래픽에선 동적 테이블에 밀린다. 위 표의 cold 열이 그 가능성의
실측 근거다 — 절대 우위가 아니라 **틈새(무상태·짧은 연결) 우위**로 읽어야 한다.
