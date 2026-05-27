/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* test_api.c — include/bcb.h 공개 API 검증 (libbcb 만 링크, 내부 헤더 미사용).
 *   test_api <prior.bcb-prior> <message-file>
 * one-shot / encoder·decoder / from_memory round-trip + 에러 경로 검사. */
#include "bcb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)sz ? (size_t)sz : 1);
    size_t n = fread(b, 1, (size_t)sz, f); fclose(f); *len = n; return b;
}

#define CHECK(cond, msg) do { if (!(cond)) { printf("  [FAIL] %s\n", msg); fails++; } \
                              else printf("  [ok]   %s\n", msg); } while (0)

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <prior> <message-file>\n", argv[0]); return 2; }
    int fails = 0;
    printf("BCB public API test (version %s)\n", bcb_version());

    size_t mlen = 0; unsigned char *msg = slurp(argv[2], &mlen);
    if (!msg || mlen == 0) { fprintf(stderr, "cannot read message %s\n", argv[2]); return 1; }

    BcbPrior *p = bcb_prior_open(argv[1]);
    CHECK(p != NULL, "bcb_prior_open");
    if (!p) return 1;
    printf("  footprint=%zu B, record_size=%d\n", bcb_prior_memory_footprint(p), bcb_prior_record_size(p));

    size_t cap = mlen + 64;
    unsigned char *comp = malloc(cap), *dec = malloc(mlen + 1);

    /* one-shot round-trip */
    ssize_t clen = bcb_compress(p, msg, mlen, comp, cap);
    CHECK(clen > 0, "bcb_compress returns >0");
    ssize_t dlen = bcb_decompress(p, comp, (size_t)clen, dec, mlen);
    CHECK(dlen == (ssize_t)mlen && memcmp(dec, msg, mlen) == 0, "one-shot round-trip lossless");
    printf("  %zu -> %zd bytes (%.2fx)\n", mlen, clen, clen ? (double)mlen / clen : 0);

    /* output-too-small error */
    ssize_t e1 = bcb_compress(p, msg, mlen, comp, 1);
    CHECK(e1 == BCB_ERR_OUTPUT_TOO_SMALL, "bcb_compress OUTPUT_TOO_SMALL");
    ssize_t e2 = bcb_decompress(p, comp, (size_t)clen, dec, 1);
    CHECK(e2 == BCB_ERR_OUTPUT_TOO_SMALL, "bcb_decompress OUTPUT_TOO_SMALL");
    CHECK(strcmp(bcb_strerror(BCB_ERR_OUTPUT_TOO_SMALL), "unknown error") != 0, "bcb_strerror");

    /* encoder/decoder handles */
    BcbEncoder *enc = bcb_encoder_new(p);
    BcbDecoder *dc = bcb_decoder_new(p);
    ssize_t c2 = bcb_encode(enc, msg, mlen, comp, cap);
    ssize_t d2 = bcb_decode(dc, comp, (size_t)c2, dec, mlen);
    CHECK(c2 == clen, "encoder matches one-shot length");
    CHECK(d2 == (ssize_t)mlen && memcmp(dec, msg, mlen) == 0, "encoder/decoder round-trip lossless");
    bcb_encoder_free(enc); bcb_decoder_free(dc);
    bcb_prior_close(p);

    /* from_memory round-trip */
    size_t plen = 0; unsigned char *pbuf = slurp(argv[1], &plen);
    BcbPrior *pm = bcb_prior_from_memory(pbuf, plen);
    CHECK(pm != NULL, "bcb_prior_from_memory");
    if (pm) {
        ssize_t c3 = bcb_compress(pm, msg, mlen, comp, cap);
        ssize_t d3 = bcb_decompress(pm, comp, (size_t)c3, dec, mlen);
        CHECK(c3 == clen && d3 == (ssize_t)mlen && memcmp(dec, msg, mlen) == 0,
              "from_memory round-trip lossless (bit-identical to mmap)");
        bcb_prior_close(pm);
    }
    free(pbuf);

    /* null/invalid prior */
    CHECK(bcb_compress(NULL, msg, mlen, comp, cap) == BCB_ERR_INVALID_PRIOR, "null prior -> INVALID_PRIOR");
    CHECK(bcb_prior_open("/no/such/file.bcb-prior") == NULL, "open missing file -> NULL");

    /* corruption detection (CRC32): 손상 시 BCB_ERR_CORRUPTED 를 정확히 반환해야 한다 */
    {
        BcbPrior *p2 = bcb_prior_open(argv[1]);
        ssize_t cc = bcb_compress(p2, msg, mlen, comp, cap);   /* crc on (기본) */
        /* 컨테이너 헤더 끝(=payload 시작) 계산: tag(1) + varint(mlen) + crc(4) */
        int vlen = 1; for (uint64_t t = (uint64_t)mlen >> 7; t; t >>= 7) vlen++;
        size_t crc_off = 1 + (size_t)vlen, payload_off = crc_off + 4;

        uint8_t save = comp[crc_off]; comp[crc_off] ^= 0xFF;   /* CRC 필드 손상 */
        CHECK(bcb_decompress(p2, comp, (size_t)cc, dec, mlen) == BCB_ERR_CORRUPTED,
              "corrupted CRC field -> BCB_ERR_CORRUPTED");
        comp[crc_off] = save;

        if ((size_t)cc > payload_off) {
            save = comp[payload_off]; comp[payload_off] ^= 0xFF;   /* payload 첫 바이트 손상 */
            ssize_t r = bcb_decompress(p2, comp, (size_t)cc, dec, mlen);
            CHECK(r == BCB_ERR_CORRUPTED, "corrupted payload -> BCB_ERR_CORRUPTED");
            comp[payload_off] = save;
        }
        ssize_t ok = bcb_decompress(p2, comp, (size_t)cc, dec, mlen);
        CHECK(ok == (ssize_t)mlen && memcmp(dec, msg, mlen) == 0, "clean input still decodes (CRC ok)");
        bcb_prior_close(p2);
    }

    free(msg); free(comp); free(dec);
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
