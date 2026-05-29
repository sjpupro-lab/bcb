/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* bcb_api.c — BCB Public API v1.0 구현 (include/bcb.h).
 *
 * 스레드 안전: 각 BcbEncoder/BcbDecoder 는 자기 BcbCodec(인스턴스 상태: window/
 * structural position)을 가진다. prior 와 LUT 는 읽기 전용 공유. 따라서 서로 다른
 * 핸들을 여러 스레드가 동시에 써도 안전하다 (prior 는 스레드 생성 전에 open).
 *
 * 컨테이너: [tag:1][varint orig_len][crc32:4 if tag&1][range payload].
 *   tag bit0 = CRC32 무결성 체크 포함 여부. 디코더가 태그로 자동 감지·검증. */
#include "bcb.h"
#include "ce_compress.h"
#include "btv3.h"
#include "bcb_prior.h"
#include <stdlib.h>
#include <string.h>

struct BcbEncoder { BcbCodec *c; int crc_on; int id_on; };
struct BcbDecoder { BcbCodec *c; };

#define BCB_TAG_CRC  0x01
#define BCB_TAG_ID   0x02

/* bitwise CRC32 (IEEE) — 테이블 없음 → lazy-init race 없음(스레드 안전). */
static uint32_t crc32_(const uint8_t *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

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

size_t bcb_compress_bound(size_t input_len) {
    /* range coder 최악 ~14bit/byte(=1.75x) + 컨테이너 헤더(tag+varint+crc4+id16<64). */
    return input_len * 2 + 64;
}

/* ── core (codec 인스턴스 기반, 전역 미사용) ── */
static ssize_t compress_core(BcbCodec *c, int crc_on, int id_on, const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t cap) {
    if (!c) return BCB_ERR_INVALID_PRIOR;
    bcb_codec_begin(c);
    CecBT bt = bcb_codec_cec_bt(c);
    CecEncoder *e = cec_enc_new(&bt);
    if (!e) return BCB_ERR_INVALID_PRIOR;
    for (size_t i = 0; i < in_len; i++) cec_enc_byte(e, in[i]);
    size_t clen = 0;
    uint8_t *comp = cec_enc_finish(e, &clen);
    cec_enc_free(e);
    if (!comp && clen) return BCB_ERR_INVALID_PRIOR;

    uint8_t hdr[32]; int hn = 0;
    hdr[hn++] = (uint8_t)((crc_on ? BCB_TAG_CRC : 0) | (id_on ? BCB_TAG_ID : 0));
    hn += varint_put(hdr + hn, (uint64_t)in_len);
    if (crc_on) {
        uint32_t cc = crc32_(in, in_len);
        hdr[hn++] = (uint8_t)(cc & 0xFF); hdr[hn++] = (uint8_t)((cc >> 8) & 0xFF);
        hdr[hn++] = (uint8_t)((cc >> 16) & 0xFF); hdr[hn++] = (uint8_t)((cc >> 24) & 0xFF);
    }
    if (id_on) {
        const unsigned char *id = bcb_codec_prior_id(c);
        memcpy(hdr + hn, id, BCB_PRIOR_ID_LEN); hn += BCB_PRIOR_ID_LEN;
    }
    if ((size_t)hn + clen > cap) { free(comp); return BCB_ERR_OUTPUT_TOO_SMALL; }
    memcpy(out, hdr, (size_t)hn);
    if (clen) memcpy(out + hn, comp, clen);
    free(comp);
    return (ssize_t)((size_t)hn + clen);
}

static ssize_t decompress_core(BcbCodec *c, const uint8_t *in, size_t in_len,
                               uint8_t *out, size_t cap) {
    if (!c) return BCB_ERR_INVALID_PRIOR;
    if (in_len < 1) return BCB_ERR_CORRUPTED;
    uint8_t tag = in[0]; size_t off = 1;
    uint64_t orig = 0;
    int hn = varint_get(in + off, in_len - off, &orig);
    if (hn <= 0) return BCB_ERR_CORRUPTED;
    off += (size_t)hn;
    uint32_t want = 0; int has_crc = tag & BCB_TAG_CRC, has_id = tag & BCB_TAG_ID;
    if (has_crc) {
        if (in_len < off + 4) return BCB_ERR_CORRUPTED;
        want = (uint32_t)in[off] | ((uint32_t)in[off+1] << 8) |
               ((uint32_t)in[off+2] << 16) | ((uint32_t)in[off+3] << 24);
        off += 4;
    }
    if (has_id) {
        if (in_len < off + BCB_PRIOR_ID_LEN) return BCB_ERR_CORRUPTED;
        const unsigned char *myid = bcb_codec_prior_id(c);
        if (!myid || memcmp(in + off, myid, BCB_PRIOR_ID_LEN) != 0) return BCB_ERR_PRIOR_ID_MISMATCH;
        off += BCB_PRIOR_ID_LEN;
    }
    if (orig > cap) return BCB_ERR_OUTPUT_TOO_SMALL;
    if (orig == 0) return 0;
    bcb_codec_begin(c);
    CecBT bt = bcb_codec_cec_bt(c);
    uint8_t *dec = cec_decompress(in + off, in_len - off, (size_t)orig, &bt);
    if (!dec) return BCB_ERR_CORRUPTED;
    if (has_crc && crc32_(dec, (size_t)orig) != want) { free(dec); return BCB_ERR_CORRUPTED; }
    memcpy(out, dec, (size_t)orig);
    free(dec);
    return (ssize_t)orig;
}

/* ── Prior ── */
BcbPrior *bcb_prior_open(const char *path) { return bcb_prior_mmap(path); }
BcbPrior *bcb_prior_from_memory(const void *data, size_t len) { return bcb_prior_from_buffer(data, len); }
size_t    bcb_prior_memory_footprint(const BcbPrior *p) { return bcb_prior_map_len(p); }
const uint8_t *bcb_prior_id(const BcbPrior *p, size_t *out_len) {
    if (out_len) *out_len = BCB_PRIOR_ID_LEN;
    return p ? (const uint8_t *)bcb_prior_id_bytes(p) : NULL;
}
/* bcb_prior_close / bcb_prior_record_size: 내부 정의 export */

/* ── One-shot (임시 codec; 각 호출 독립 → 스레드 안전) ── */
ssize_t bcb_compress(BcbPrior *p, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
    if (!p) return BCB_ERR_INVALID_PRIOR;
    BcbCodec *c = bcb_codec_new(p);
    if (!c) return BCB_ERR_INVALID_PRIOR;
    ssize_t r = compress_core(c, 1, 0, in, in_len, out, cap);
    bcb_codec_free(c);
    return r;
}
ssize_t bcb_decompress(BcbPrior *p, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
    if (!p) return BCB_ERR_INVALID_PRIOR;
    BcbCodec *c = bcb_codec_new(p);
    if (!c) return BCB_ERR_INVALID_PRIOR;
    ssize_t r = decompress_core(c, in, in_len, out, cap);
    bcb_codec_free(c);
    return r;
}

/* ── Encoder / Decoder (핸들마다 codec → 스레드별 핸들 동시 사용 안전) ── */
BcbEncoder *bcb_encoder_new(BcbPrior *p) {
    if (!p) return NULL;
    BcbEncoder *e = (BcbEncoder *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->c = bcb_codec_new(p); e->crc_on = 1;
    if (!e->c) { free(e); return NULL; }
    return e;
}
ssize_t bcb_encode(BcbEncoder *e, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
    if (!e) return BCB_ERR_INVALID_PRIOR;
    return compress_core(e->c, e->crc_on, e->id_on, in, in_len, out, cap);
}
void bcb_encoder_set_checksum(BcbEncoder *e, int on) { if (e) e->crc_on = on ? 1 : 0; }
void bcb_encoder_set_prior_id(BcbEncoder *e, int on) { if (e) e->id_on = on ? 1 : 0; }
void bcb_encoder_free(BcbEncoder *e) { if (e) { bcb_codec_free(e->c); free(e); } }

BcbDecoder *bcb_decoder_new(BcbPrior *p) {
    if (!p) return NULL;
    BcbDecoder *d = (BcbDecoder *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->c = bcb_codec_new(p);
    if (!d->c) { free(d); return NULL; }
    return d;
}
ssize_t bcb_decode(BcbDecoder *d, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
    if (!d) return BCB_ERR_INVALID_PRIOR;
    return decompress_core(d->c, in, in_len, out, cap);
}
void bcb_decoder_free(BcbDecoder *d) { if (d) { bcb_codec_free(d->c); free(d); } }

/* ── misc ── */
const char *bcb_strerror(BcbStatus s) {
    switch (s) {
        case BCB_OK:                 return "ok";
        case BCB_ERR_INVALID_PRIOR:  return "invalid or null prior";
        case BCB_ERR_OUTPUT_TOO_SMALL: return "output buffer too small";
        case BCB_ERR_CORRUPTED:      return "corrupted input";
        case BCB_ERR_VERSION:        return "unsupported prior file version";
        case BCB_ERR_PRIOR_ID_MISMATCH: return "prior id mismatch (wrong prior)";
    }
    return "unknown error";
}
const char *bcb_version(void) { return "0.2.0"; }
