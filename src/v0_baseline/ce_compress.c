/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* ce_compress.c — implementation */
#include "ce_compress.h"
#include <stdlib.h>
#include <string.h>

/* ── Range coder 내부 상태 ──────────────────────── */
struct CecEncoder {
    uint32_t low, high;
    uint32_t pending;
    uint8_t  cur_byte;        /* 8개 비트 모으는 버퍼 */
    uint8_t  cur_bits;        /* 0..7 */
    uint8_t *out;
    size_t   out_len, out_cap;
    const CecBT *bt;
};

struct CecDecoder {
    uint32_t low, high, code;
    const uint8_t *in;
    size_t in_len, in_pos;
    uint8_t cur_byte;
    uint8_t cur_bits;
    const CecBT *bt;
};

/* ── 내부: 비트 출력 ────────────────────────────── */
static void cec_emit_bit_(CecEncoder *e, int bit) {
    e->cur_byte = (uint8_t)((e->cur_byte << 1) | (bit & 1));
    e->cur_bits++;
    if (e->cur_bits == 8) {
        if (e->out_len + 1 > e->out_cap) {
            size_t nc = e->out_cap ? e->out_cap * 2 : 256;
            e->out = (uint8_t*)realloc(e->out, nc);
            e->out_cap = nc;
        }
        e->out[e->out_len++] = e->cur_byte;
        e->cur_byte = 0;
        e->cur_bits = 0;
    }
}

static void cec_flush_(CecEncoder *e, int bit) {
    cec_emit_bit_(e, bit);
    while (e->pending > 0) {
        cec_emit_bit_(e, 1 - bit);
        e->pending--;
    }
}

static void cec_renorm_enc_(CecEncoder *e) {
    while (1) {
        if (e->high < (1u<<31)) cec_flush_(e, 0);
        else if (e->low >= (1u<<31)) {
            cec_flush_(e, 1);
            e->low  -= (1u<<31);
            e->high -= (1u<<31);
        } else if (e->low >= (1u<<30) && e->high < (3u<<30)) {
            e->pending++;
            e->low  -= (1u<<30);
            e->high -= (1u<<30);
        } else break;
        e->low <<= 1;
        e->high = (e->high << 1) | 1;
    }
}

static void cec_enc_sym_(CecEncoder *e, uint32_t cl, uint32_t ch, uint32_t tot) {
    uint64_t r = (uint64_t)(e->high - e->low) + 1;
    e->high = e->low + (uint32_t)((r * ch) / tot) - 1;
    e->low  = e->low + (uint32_t)((r * cl) / tot);
    cec_renorm_enc_(e);
}

/* ── 인코더 ───────────────────────────────────────────── */
CecEncoder *cec_enc_new(const CecBT *bt) {
    CecEncoder *e = (CecEncoder*)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->low = 0; e->high = 0xFFFFFFFFu;
    e->bt = bt;
    return e;
}

void cec_enc_free(CecEncoder *e) {
    if (!e) return;
    free(e->out);
    free(e);
}

void cec_enc_byte(CecEncoder *e, uint8_t byte) {
    uint32_t cum[257];
    e->bt->distribution(cum, CEC_RC_SCALE, e->bt->user);
    cec_enc_sym_(e, cum[byte], cum[byte+1], cum[256]);
    e->bt->train(byte, e->bt->user);
}

uint8_t *cec_enc_finish(CecEncoder *e, size_t *out_len) {
    e->pending++;
    cec_flush_(e, (e->low < (1u<<30)) ? 0 : 1);
    while (e->cur_bits > 0) cec_emit_bit_(e, 0);
    uint8_t *out = e->out;
    *out_len = e->out_len;
    e->out = NULL;
    e->out_len = 0;
    return out;
}

/* ── 디코더 ───────────────────────────────────────────── */
static int cec_read_bit_(CecDecoder *d) {
    if (d->cur_bits == 0) {
        if (d->in_pos < d->in_len) d->cur_byte = d->in[d->in_pos++];
        else d->cur_byte = 0;
        d->cur_bits = 8;
    }
    int b = (d->cur_byte >> 7) & 1;
    d->cur_byte <<= 1;
    d->cur_bits--;
    return b;
}

CecDecoder *cec_dec_new(const uint8_t *data, size_t len, const CecBT *bt) {
    CecDecoder *d = (CecDecoder*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->in = data; d->in_len = len; d->in_pos = 0;
    d->low = 0; d->high = 0xFFFFFFFFu;
    d->bt = bt;
    for (int i=0; i<32; i++)
        d->code = (d->code << 1) | cec_read_bit_(d);
    return d;
}

void cec_dec_free(CecDecoder *d) { free(d); }

static void cec_renorm_dec_(CecDecoder *d) {
    while (1) {
        if (d->high < (1u<<31)) { /* nothing */ }
        else if (d->low >= (1u<<31)) {
            d->code -= (1u<<31);
            d->low  -= (1u<<31);
            d->high -= (1u<<31);
        } else if (d->low >= (1u<<30) && d->high < (3u<<30)) {
            d->code -= (1u<<30);
            d->low  -= (1u<<30);
            d->high -= (1u<<30);
        } else break;
        d->low <<= 1;
        d->high = (d->high << 1) | 1;
        d->code = (d->code << 1) | cec_read_bit_(d);
    }
}

uint8_t cec_dec_byte(CecDecoder *d) {
    uint32_t cum[257];
    d->bt->distribution(cum, CEC_RC_SCALE, d->bt->user);
    uint64_t r = (uint64_t)(d->high - d->low) + 1;
    uint32_t target = (uint32_t)(((uint64_t)(d->code - d->low + 1) * cum[256] - 1) / r);
    /* binary search */
    int lo = 0, hi = 256;
    while (lo + 1 < hi) {
        int m = (lo + hi) >> 1;
        if (cum[m] <= target) lo = m; else hi = m;
    }
    uint8_t byte = (uint8_t)lo;
    d->high = d->low + (uint32_t)((r * cum[byte+1]) / cum[256]) - 1;
    d->low  = d->low + (uint32_t)((r * cum[byte])   / cum[256]);
    cec_renorm_dec_(d);
    d->bt->train(byte, d->bt->user);
    return byte;
}

/* ── 편의 함수 ────────────────────────────────────────── */
uint8_t *cec_compress(const uint8_t *data, size_t len, const CecBT *bt, size_t *out_len) {
    CecEncoder *e = cec_enc_new(bt);
    if (!e) return NULL;
    for (size_t i=0; i<len; i++) cec_enc_byte(e, data[i]);
    uint8_t *out = cec_enc_finish(e, out_len);
    cec_enc_free(e);
    return out;
}

uint8_t *cec_decompress(const uint8_t *data, size_t len, size_t orig_len, const CecBT *bt) {
    CecDecoder *d = cec_dec_new(data, len, bt);
    if (!d) return NULL;
    uint8_t *out = (uint8_t*)malloc(orig_len);
    if (!out) { cec_dec_free(d); return NULL; }
    for (size_t i=0; i<orig_len; i++) out[i] = cec_dec_byte(d);
    cec_dec_free(d);
    return out;
}

/* ── 내장 BT (bt_model 기반) — cec_default_bt() ──────────────────────────
 * 이 글루는 v0 reference BT(bt_model.c, bt_v4_*)를 CecBT 로 감싼다. CLI·테스트
 * 전용이며 제품 경로(공개 API)는 쓰지 않는다. range coder 가 v0 BT 에 링크
 * 의존하지 않도록(코어 라이브러리에서 bt_model.c 제외 가능) bt_model.c 로 옮겼다. */

