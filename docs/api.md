# BCB Public Library API v0.2.0 (`include/bcb.h`)

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
const char *bcb_strerror(BcbStatus); const char *bcb_version(void);  /* "0.2.0" */
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

## 무결성 체크섬 (CRC32)

`bcb_compress` / `bcb_encode` 는 기본으로 메시지마다 4바이트 CRC32 를 컨테이너에 넣는다. 디코더는
컨테이너 태그로 자동 감지·검증하여, 손상 입력에 **`BCB_ERR_CORRUPTED`** 를 반환한다(`make api-test`
에서 CRC 필드·payload 손상 모두 검출 확인). 컨테이너: `[tag:1][varint orig_len][crc32:4][payload]`.

작은 메시지에서 4바이트 오버헤드가 부담이면 encoder 에서 끄라(최대 압축비):

```c
BcbEncoder *e = bcb_encoder_new(p);
bcb_encoder_set_checksum(e, 0);   /* CRC off → tag bit0=0, 디코더가 자동으로 검증 생략 */
```

## 스레드 안전성

각 `BcbEncoder`/`BcbDecoder` 는 자기 인스턴스 상태(window/structural position)를 가진다.
prior(mmap)와 LUT 는 **읽기 전용 공유**다. 따라서 **서로 다른 핸들을 여러 스레드가 동시에 써도
안전**하다 (같은 prior 를 공유해도 됨). 검증: `make threads-test` (8스레드 동시 encode/decode 무손실,
landmark·structural prior 모두). 단:
- prior 는 **스레드 생성 전에** `bcb_prior_open` 할 것 (LUT 가 그때 1회 생성된다).
- **하나의 핸들**을 여러 스레드가 동시에 쓰는 것은 안 된다 (핸들=인스턴스 상태). 스레드마다 핸들을 따로.
- one-shot `bcb_compress`/`bcb_decompress` 는 호출마다 임시 codec 을 만들어 그 자체로 스레드 안전.

## 계약 / 한계 (정직하게)

- **무손실**: encode→decode 는 원본을 비트 단위 복원한다 (`make api-test`, `make threads-test`).
- **prior 공유 필수**: encode/decode 양쪽이 동일 `.bcb-prior` 를 써야 한다. 다르면 복원 불가.
- **버전**: prior 파일 포맷 버전이 맞아야 로드된다(`bcb_prior_open` 이 NULL 반환). semver `bcb_version()`.
- `bcb_compress` 출력 용량은 안전하게 `in_len + 64` 이상을 권장(작은 메시지 최악 시 확장 + 헤더 여지).

## ABI

C API, semver (현재 0.2.0). 위 시그니처·상태코드는 안정 유지. 내부 prior 포맷(version 3)은
독립적으로 진화할 수 있으나, 구 prior 는 재빌드가 필요할 수 있다(`bcb_prior_open` NULL 로 감지).
