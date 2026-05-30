/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* bcb.h — BCB Public API v0.2 (C API, semver).
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

/* ssize_t: POSIX/MinGW provide it via <sys/types.h>; MSVC does not. */
#if defined(_MSC_VER)
  #include <BaseTsd.h>
  #ifndef _SSIZE_T_DEFINED
    #define _SSIZE_T_DEFINED
    typedef SSIZE_T ssize_t;
  #endif
#else
  #include <sys/types.h>   /* ssize_t */
#endif

/* ── 심볼 가시성 / symbol visibility ──────────────────────────
 * 공유 라이브러리 빌드·소비 시에만 declspec/visibility 를 단다. 하나의 결정 로직:
 *   - BCB_SHARED        : 공유 라이브러리를 빌드/소비 중 (CMake 가 PUBLIC 으로 설정).
 *   - bcb_shared_EXPORTS: CMake 가 bcb_shared 타깃을 *빌드할 때만* 자동 정의 → dllexport.
 *     그 외(소비자)는 dllimport. 정적 라이브러리·Makefile·직접 컴파일(BCB_SHARED 미정의)은
 *     장식 없음. 선언부(bcb.h/bcb_prior.h)와 정의부가 모두 이 BCB_API 만 쓴다(declspec 하드코딩 금지). */
#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(BCB_SHARED)
    #if defined(bcb_shared_EXPORTS)
      #define BCB_API __declspec(dllexport)
    #else
      #define BCB_API __declspec(dllimport)
    #endif
  #else
    #define BCB_API
  #endif
#elif defined(BCB_SHARED) && defined(bcb_shared_EXPORTS)
  #define BCB_API __attribute__((visibility("default")))
#else
  #define BCB_API
#endif

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
    BCB_ERR_CORRUPTED      = -3,   /* 입력 컨테이너 손상 (CRC 불일치 포함) */
    BCB_ERR_VERSION        = -4,   /* prior 파일 포맷 버전 불일치 */
    BCB_ERR_PRIOR_ID_MISMATCH = -5,/* 압축본의 prior id 가 디코딩 prior 와 불일치 */
} BcbStatus;

#define BCB_PRIOR_ID_LEN 16        /* prior id 길이 (SHA-256 앞 16바이트) */

#ifndef BCB_PRIOR_TYPE_DEFINED
#define BCB_PRIOR_TYPE_DEFINED
typedef struct BcbPrior BcbPrior;
#endif
typedef struct BcbEncoder BcbEncoder;
typedef struct BcbDecoder BcbDecoder;

/* ── Prior — 학습된 모델 ─────────────────────────────────── */
/* 파일을 mmap 으로 연다 (RAM 거의 0, 즉시 시작). 실패 시 NULL. */
BCB_API BcbPrior *bcb_prior_open(const char *path);
/* 메모리 이미지를 복사·소유해 연다 (mmap 불가 환경). 실패 시 NULL. */
BCB_API BcbPrior *bcb_prior_from_memory(const void *data, size_t len);
BCB_API void      bcb_prior_close(BcbPrior *p);
/* prior 의 대략적 메모리 footprint(bytes): mmap 매핑 길이(+RAM 보조 테이블). */
BCB_API size_t    bcb_prior_memory_footprint(const BcbPrior *p);
/* record schema 가 있으면 record_size(>0), 없으면 0. */
BCB_API int       bcb_prior_record_size(const BcbPrior *p);
/* prior 의 고유 id (SHA-256(prior 이미지) 앞 16바이트). *out_len = BCB_PRIOR_ID_LEN.
 * 같은 학습으로 만든 prior 는 같은 id. encode/decode 양쪽 prior 일치 확인용. */
BCB_API const uint8_t *bcb_prior_id(const BcbPrior *p, size_t *out_len);

/* 입력 input_len 에 대해 출력 버퍼가 반드시 충분한 크기(상한). zstd/lz4 의 _bound 패턴.
 * BCB 는 비압축성 입력에서 팽창할 수 있으므로(range coder 최악 ~14bit/byte) 보수적 상한. */
BCB_API size_t bcb_compress_bound(size_t input_len);

/* ── One-shot (encoder/decoder 할당 없이) ─────────────────── */
/* in[0..in_len) 를 out 으로 압축. 성공 시 출력 바이트수, 실패 시 음수 BcbStatus.
 * 출력은 자기 기술적(원본 길이 포함)이라 bcb_decompress 가 길이를 안다. */
BCB_API ssize_t bcb_compress(BcbPrior *p, const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t out_capacity);
/* bcb_compress 출력을 복원. 성공 시 원본 바이트수, 실패 시 음수 BcbStatus. */
BCB_API ssize_t bcb_decompress(BcbPrior *p, const uint8_t *in, size_t in_len,
                               uint8_t *out, size_t out_capacity);

/* ── Encoder / Decoder 핸들 (prior 재사용; 메시지마다 reset) ── */
BCB_API BcbEncoder *bcb_encoder_new(BcbPrior *p);
BCB_API ssize_t     bcb_encode(BcbEncoder *e, const uint8_t *input, size_t input_len,
                               uint8_t *output, size_t output_capacity);
/* 메시지마다 4바이트 CRC32 무결성 체크 on/off (기본 on). 작은 메시지에서 최대 압축비를
 * 원하면 off. 디코더는 컨테이너 태그로 자동 감지하므로 별도 설정 불필요. */
BCB_API void        bcb_encoder_set_checksum(BcbEncoder *e, int on);
/* 압축본에 prior id(16B)를 박는다 (기본 off). on 이면 디코더가 prior 불일치를
 * BCB_ERR_PRIOR_ID_MISMATCH 로 즉시 잡는다. 작은 메시지엔 16B 오버헤드 주의. */
BCB_API void        bcb_encoder_set_prior_id(BcbEncoder *e, int on);
BCB_API void        bcb_encoder_free(BcbEncoder *e);

BCB_API BcbDecoder *bcb_decoder_new(BcbPrior *p);
BCB_API ssize_t     bcb_decode(BcbDecoder *d, const uint8_t *input, size_t input_len,
                               uint8_t *output, size_t output_capacity);
BCB_API void        bcb_decoder_free(BcbDecoder *d);

/* ── 잡 ──────────────────────────────────────────────────── */
BCB_API const char *bcb_strerror(BcbStatus s);   /* 에러 코드 → 사람이 읽는 문자열 */
BCB_API const char *bcb_version(void);            /* semver 문자열 "0.2.0" */

#ifdef __cplusplus
}
#endif
#endif /* BCB_H */
