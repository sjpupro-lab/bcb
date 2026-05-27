/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* bcb_api.c — BCB Public API v1.0 구현 (include/bcb.h).
 * 내부 v0/v3/v5 코덱 위의 얇은 안정 facade. 컨테이너: varint(orig_len) + payload. */
#include "bcb.h"
#include "ce_compress.h"
#include "btv3.h"
#include "bcb_prior.h"
#include <stdlib.h>
#include <string.h>

struct BcbEncoder { BcbPrior *p; };
struct BcbDecoder { BcbPrior *p; };

/* ── varint (LEB128) ── */
static int varint_put(uint8_t *buf, uint64_t v) {
    int n = 0;
    do { uint8_t b = v & 0x7F; v >>= 7; if (v) b |= 0x80; buf[n++] = b; } while (v);
    return n;
}
static int varint_get(const uint8_t *buf, size_t len, uint64_t *out) {
    uint64_t v = 0; int shift = 0;
    for (size_t i = 0; i < len && i < 10; i++) {
        uint8_t b = buf[i];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *out = v; return (int)(i + 1); }
        shift += 7;
    }
    return -1;
}

/* ── core ── */
static ssize_t compress_core(BcbPrior *p, const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t cap) {
    if (!p) return BCB_ERR_INVALID_PRIOR;
    CecBT bt = bcb_prior_cec_bt(p);     /* structural: msg_begin; landmark/BT: attach */
    bt_v3_reset_window();
    CecEncoder *e = cec_enc_new(&bt);
    if (!e) return BCB_ERR_INVALID_PRIOR;
    for (size_t i = 0; i < in_len; i++) cec_enc_byte(e, in[i]);
    size_t clen = 0;
    uint8_t *comp = cec_enc_finish(e, &clen);
    cec_enc_free(e);
    if (!comp && clen) return BCB_ERR_INVALID_PRIOR;

    uint8_t hdr[10];
    int hn = varint_put(hdr, (uint64_t)in_len);
    if ((size_t)hn + clen > cap) { free(comp); return BCB_ERR_OUTPUT_TOO_SMALL; }
    memcpy(out, hdr, (size_t)hn);
    if (clen) memcpy(out + hn, comp, clen);
    free(comp);
    return (ssize_t)((size_t)hn + clen);
}

static ssize_t decompress_core(BcbPrior *p, const uint8_t *in, size_t in_len,
                               uint8_t *out, size_t cap) {
    if (!p) return BCB_ERR_INVALID_PRIOR;
    uint64_t orig = 0;
    int hn = varint_get(in, in_len, &orig);
    if (hn <= 0) return BCB_ERR_CORRUPTED;
    if (orig > cap) return BCB_ERR_OUTPUT_TOO_SMALL;
    if (orig == 0) return 0;
    CecBT bt = bcb_prior_cec_bt(p);
    bt_v3_reset_window();
    uint8_t *dec = cec_decompress(in + hn, in_len - (size_t)hn, (size_t)orig, &bt);
    if (!dec) return BCB_ERR_CORRUPTED;
    memcpy(out, dec, (size_t)orig);
    free(dec);
    return (ssize_t)orig;
}

/* ── Prior ── */
BcbPrior *bcb_prior_open(const char *path) { return bcb_prior_mmap(path); }
BcbPrior *bcb_prior_from_memory(const void *data, size_t len) { return bcb_prior_from_buffer(data, len); }
/* bcb_prior_close / bcb_prior_record_size: 내부 정의 그대로 export */
size_t bcb_prior_memory_footprint(const BcbPrior *p) { return bcb_prior_map_len(p); }

/* ── One-shot ── */
ssize_t bcb_compress(BcbPrior *p, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
    return compress_core(p, in, in_len, out, cap);
}
ssize_t bcb_decompress(BcbPrior *p, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
    return decompress_core(p, in, in_len, out, cap);
}

/* ── Encoder / Decoder ── */
BcbEncoder *bcb_encoder_new(BcbPrior *p) {
    if (!p) return NULL;
    BcbEncoder *e = (BcbEncoder *)calloc(1, sizeof *e); if (e) e->p = p; return e;
}
ssize_t bcb_encode(BcbEncoder *e, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
    if (!e) return BCB_ERR_INVALID_PRIOR;
    return compress_core(e->p, in, in_len, out, cap);
}
void bcb_encoder_free(BcbEncoder *e) { free(e); }

BcbDecoder *bcb_decoder_new(BcbPrior *p) {
    if (!p) return NULL;
    BcbDecoder *d = (BcbDecoder *)calloc(1, sizeof *d); if (d) d->p = p; return d;
}
ssize_t bcb_decode(BcbDecoder *d, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
    if (!d) return BCB_ERR_INVALID_PRIOR;
    return decompress_core(d->p, in, in_len, out, cap);
}
void bcb_decoder_free(BcbDecoder *d) { free(d); }

/* ── misc ── */
const char *bcb_strerror(BcbStatus s) {
    switch (s) {
        case BCB_OK:                 return "ok";
        case BCB_ERR_INVALID_PRIOR:  return "invalid or null prior";
        case BCB_ERR_OUTPUT_TOO_SMALL: return "output buffer too small";
        case BCB_ERR_CORRUPTED:      return "corrupted input";
        case BCB_ERR_VERSION:        return "unsupported prior file version";
    }
    return "unknown error";
}
const char *bcb_version(void) { return "1.0.0"; }
