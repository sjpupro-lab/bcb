# BCB Public Library API v1.0 (`include/bcb.h`)

BCB 를 호시 시스템의 *컴포넌트* 로 쓰기 위한 안정 C API. 정적 라이브러리 `build/libbcb.a`
+ 헤더 `include/bcb.h` 만으로 링크된다 (내부 v0/v3/v5 헤더 불필요). 검증: `make api-test`.

## 빌드

```sh
make build/libbcb.a          # 정적 라이브러리
cc -Iinclude my_app.c build/libbcb.a -lm -o my_app
```

## 모델

- **Prior** = 미리 학습된 공유 모델 (`bcb-prior-build` 로 생성한 `.bcb-prior`). 인코더·디코더가
  같은 prior 를 공유한다. 텍스트형은 `--landmark-k`, 고정 레코드 binary 는 `--schema-record-size R`.
- 압축 단위는 **메시지**(작은 byte 열). `bcb_compress` 출력은 자기 기술적(원본 길이 varint 포함)이라
  `bcb_decompress` 가 길이를 안다.

## API

```c
/* Prior */
BcbPrior *bcb_prior_open(const char *path);              /* mmap (즉시 시작, RAM 거의 0) */
BcbPrior *bcb_prior_from_memory(const void *data, size_t len);  /* 복사·소유 (mmap 불가 환경) */
void      bcb_prior_close(BcbPrior *p);
size_t    bcb_prior_memory_footprint(const BcbPrior *p); /* prior 이미지 크기(bytes) */
int       bcb_prior_record_size(const BcbPrior *p);      /* schema 있으면 record_size, 없으면 0 */

/* One-shot (핸들 할당 없이) — 성공: 바이트수(>=0), 실패: 음수 BcbStatus */
ssize_t bcb_compress(BcbPrior*, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap);
ssize_t bcb_decompress(BcbPrior*, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap);

/* Encoder / Decoder 핸들 (prior 재사용; 호출마다 메시지 1개, 내부 reset) */
BcbEncoder *bcb_encoder_new(BcbPrior*);  ssize_t bcb_encode(BcbEncoder*, ...);  void bcb_encoder_free(BcbEncoder*);
BcbDecoder *bcb_decoder_new(BcbPrior*);  ssize_t bcb_decode(BcbDecoder*, ...);  void bcb_decoder_free(BcbDecoder*);

/* 잡 */
const char *bcb_strerror(BcbStatus); const char *bcb_version(void);  /* "1.0.0" */
```

상태 코드: `BCB_OK(0)`, `BCB_ERR_INVALID_PRIOR(-1)`, `BCB_ERR_OUTPUT_TOO_SMALL(-2)`,
`BCB_ERR_CORRUPTED(-3)`, `BCB_ERR_VERSION(-4)`.

## 사용 예

```c
#include "bcb.h"
BcbPrior *p = bcb_prior_open("sensors.bcb-prior");
uint8_t out[512];
ssize_t n = bcb_compress(p, msg, msg_len, out, sizeof out);   /* n>0: 압축 길이 */
/* ... 전송/저장 ... */
uint8_t back[512];
ssize_t m = bcb_decompress(p, out, (size_t)n, back, sizeof back);  /* m == msg_len */
bcb_prior_close(p);
```

## 계약 / 한계 (정직하게)

- **무손실**: encode→decode 는 원본을 비트 단위 복원한다 (round-trip 검증: `make api-test`).
  단 메시지 자체 무결성 체크섬은 없다 — 손상 입력은 잘못된 출력이 될 수 있다(컨테이너 길이만 검증).
- **prior 공유 필수**: encode/decode 양쪽이 동일 `.bcb-prior` 를 써야 한다. 다르면 복원 불가.
- **스레드 안전성**: BCB 코덱은 단일 전역 상태를 쓴다. 한 시점에 하나의 (en|de)code 만 진행 가능.
  멀티스레드/멀티 prior 동시 사용은 외부 직렬화나 프로세스 분리가 필요하다 (현재 한계).
- **버전**: prior 파일 포맷 버전이 맞아야 로드된다(`bcb_prior_open` 이 NULL 반환). semver `bcb_version()`.
- `bcb_compress` 출력 용량은 안전하게 `in_len + 64` 이상을 권장(작은 메시지 최악 시 확장 여지).

## ABI

C API, semver. v1.x 동안 위 시그니처·상태코드는 안정 유지. 내부 prior 포맷(version 3)은
독립적으로 진화할 수 있으나, 구 prior 는 재빌드가 필요할 수 있다(`bcb_prior_open` NULL 로 감지).
