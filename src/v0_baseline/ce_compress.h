/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* ce_compress.h
 * SSS 압축 모듈 — BT 기반 무손실 압축
 *
 * 아키텍처:
 *   인코드: data → BT prior + range coder → compressed bytes
 *   디코드: compressed + same BT → data
 *
 * BT는 ce_storage / ce_storage_add_typed 와 연결될 자리.
 * 사용자가 학습 코퍼스를 결정하면, 같은 코퍼스로 양쪽 학습 →
 * 무손실 압축본은 더 작아짐 (확률분포가 데이터에 맞춰짐).
 *
 * Phase 1 단독 사용 가능 (BT context window 내장).
 * Phase 2 ce_storage 연결 (외부 분포 공급).
 */
#ifndef CE_COMPRESS_H
#define CE_COMPRESS_H

#include <stddef.h>
#include <stdint.h>

/* ── 상수 ─────────────────────────────────────── */
#define CEC_RC_SCALE       (1u << 14)   /* 16384 — 분포 정규화 단위 */
#define CEC_MAX_CTX_DEPTH  24            /* 최대 context 길이 (n-gram) */

/* ── 인코더 / 디코더 상태 (불투명) ─────────────── */
typedef struct CecEncoder CecEncoder;
typedef struct CecDecoder CecDecoder;

/* ── BT 인터페이스 (외부 주입 가능) ────────────── */
typedef struct {
    /* 256바이트 누적 확률분포 출력 (cum[0..256], cum[256]=scale) */
    void (*distribution)(uint32_t *cum, uint32_t scale, void *user);
    /* 학습: 1바이트 관찰 후 내부 상태 업데이트 */
    void (*train)(uint8_t byte, void *user);
    void *user;
} CecBT;

/* ── 인코더 ────────────────────────────────────── */
CecEncoder *cec_enc_new(const CecBT *bt);
void        cec_enc_free(CecEncoder *e);
/* 1바이트 인코드 (내부에 누적). buffer 부족시 자동 확장 */
void        cec_enc_byte(CecEncoder *e, uint8_t byte);
/* 종료 + 출력 버퍼 회수. 호출자가 free 책임 */
uint8_t    *cec_enc_finish(CecEncoder *e, size_t *out_len);

/* ── 디코더 ────────────────────────────────────── */
CecDecoder *cec_dec_new(const uint8_t *data, size_t len, const CecBT *bt);
void        cec_dec_free(CecDecoder *d);
/* 1바이트 디코드 (BT 동시 학습) */
uint8_t     cec_dec_byte(CecDecoder *d);

/* ── 편의 함수 — 한 번에 전체 인코드/디코드 ────── */
uint8_t *cec_compress(const uint8_t *data, size_t len,
                      const CecBT *bt, size_t *out_len);
uint8_t *cec_decompress(const uint8_t *data, size_t len, size_t orig_len,
                        const CecBT *bt);

/* ── 내장 BT (선택적) — bt_model 기반 ───────────── */
CecBT cec_default_bt(void);     /* 매 호출마다 초기화된 BT 반환 */

#endif /* CE_COMPRESS_H */
