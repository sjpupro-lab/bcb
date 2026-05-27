/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* bcb.h — BCB Public API v1.0 (stable).
 *
 * BCB 는 "공유 prior 를 가진 작은 메시지" 무손실 압축기다. 인코더와 디코더가 같은
 * 학습된 prior(.bcb-prior)를 공유하고, 각 메시지를 그 prior 기준으로 압축한다.
 * prior 는 bcb-prior-build 로 미리 만든다 (텍스트형: --landmark-k, 고정 레코드
 * binary: --schema-record-size).
 *
 * 스레드 안전성: 각 BcbEncoder/BcbDecoder 가 자기 상태를 가진다. prior 와 LUT 는
 * 읽기 전용 공유이므로, 서로 다른 핸들을 여러 스레드가 동시에 써도 안전하다(같은
 * prior 공유 가능). prior 는 스레드 생성 전에 open 할 것. 하나의 핸들을 여러 스레드가
 * 동시에 쓰지는 말 것(핸들=인스턴스 상태). 자세히는 docs/api.md.
 *
 * 무결성: bcb_compress/bcb_encode 는 기본으로 CRC32 를 넣어 손상 입력에 BCB_ERR_CORRUPTED
 * 를 반환한다 (bcb_encoder_set_checksum 으로 off 가능).
 */
#ifndef BCB_H
#define BCB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

#define BCB_VERSION_MAJOR 0
#define BCB_VERSION_MINOR 2
#define BCB_VERSION_PATCH 0

/* 상태/에러 코드. (en|de)code 류는 성공 시 출력 바이트수(>=0), 실패 시 음수 BcbStatus. */
typedef enum {
    BCB_OK = 0,
    BCB_ERR_INVALID_PRIOR  = -1,   /* prior NULL / 열기 실패 */
    BCB_ERR_OUTPUT_TOO_SMALL = -2, /* 출력 버퍼 용량 부족 */
    BCB_ERR_CORRUPTED      = -3,   /* 입력 컨테이너 손상 */
    BCB_ERR_VERSION        = -4,   /* prior 파일 포맷 버전 불일치 */
} BcbStatus;

#ifndef BCB_PRIOR_TYPE_DEFINED
#define BCB_PRIOR_TYPE_DEFINED
typedef struct BcbPrior BcbPrior;
#endif
typedef struct BcbEncoder BcbEncoder;
typedef struct BcbDecoder BcbDecoder;

/* ── Prior — 학습된 모델 ─────────────────────────────────── */
/* 파일을 mmap 으로 연다 (RAM 거의 0, 즉시 시작). 실패 시 NULL. */
BcbPrior *bcb_prior_open(const char *path);
/* 메모리 이미지를 복사·소유해 연다 (mmap 불가 환경). 실패 시 NULL. */
BcbPrior *bcb_prior_from_memory(const void *data, size_t len);
void      bcb_prior_close(BcbPrior *p);
/* prior 의 대략적 메모리 footprint(bytes): mmap 매핑 길이(+RAM 보조 테이블). */
size_t    bcb_prior_memory_footprint(const BcbPrior *p);
/* record schema 가 있으면 record_size(>0), 없으면 0. */
int       bcb_prior_record_size(const BcbPrior *p);

/* ── One-shot (encoder/decoder 할당 없이) ─────────────────── */
/* in[0..in_len) 를 out 으로 압축. 성공 시 출력 바이트수, 실패 시 음수 BcbStatus.
 * 출력은 자기 기술적(원본 길이 포함)이라 bcb_decompress 가 길이를 안다. */
ssize_t bcb_compress(BcbPrior *p, const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t out_capacity);
/* bcb_compress 출력을 복원. 성공 시 원본 바이트수, 실패 시 음수 BcbStatus. */
ssize_t bcb_decompress(BcbPrior *p, const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_capacity);

/* ── Encoder / Decoder 핸들 (prior 재사용; 메시지마다 reset) ── */
BcbEncoder *bcb_encoder_new(BcbPrior *p);
ssize_t     bcb_encode(BcbEncoder *e, const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t output_capacity);
/* 메시지마다 4바이트 CRC32 무결성 체크 on/off (기본 on). 작은 메시지에서 최대 압축비를
 * 원하면 off. 디코더는 컨테이너 태그로 자동 감지하므로 별도 설정 불필요. */
void        bcb_encoder_set_checksum(BcbEncoder *e, int on);
void        bcb_encoder_free(BcbEncoder *e);

BcbDecoder *bcb_decoder_new(BcbPrior *p);
ssize_t     bcb_decode(BcbDecoder *d, const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t output_capacity);
void        bcb_decoder_free(BcbDecoder *d);

/* ── 잡 ──────────────────────────────────────────────────── */
const char *bcb_strerror(BcbStatus s);   /* 에러 코드 → 사람이 읽는 문자열 */
const char *bcb_version(void);            /* semver 문자열 "1.0.0" */

#ifdef __cplusplus
}
#endif
#endif /* BCB_H */
