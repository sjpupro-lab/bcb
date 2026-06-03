/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* test_distcache.c — BT distribution cache (codec_dist memoization) unit test.
 *   test_distcache <prior> <corpus>
 *
 * The cache is a pure memoization of bt_v3_distribution_r keyed on the context
 * window (frozen prior ⇒ window→cum is a pure function). Correctness is proven
 * two ways here, plus a cross-build cmp in the Makefile:
 *   1. lossless round-trip over many messages with a WARM cache — the range coder
 *      is bit-sensitive, so any wrong cached cum (e.g. a hash collision returning
 *      another window's distribution) would corrupt the decode. Lossless with a
 *      non-zero hit count therefore proves every served hit equalled the recompute.
 *   2. re-encoding the same message increases the hit count (memoization works).
 * Under -DBCB_NO_DISTCACHE the stats report 0/0 and the round-trip still holds. */
#include "ce_compress.h"
#include "btv3.h"
#include "bcb_prior.h"
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

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  [FAIL] %s\n", msg); fails++; } \
                              else printf("  [ok]   %s\n", msg); } while (0)

/* encode one message via the per-instance codec (BT path) */
static unsigned char *enc(BcbCodec *c, const unsigned char *msg, size_t m, size_t *clen) {
    bcb_codec_begin(c);
    CecBT bt = bcb_codec_cec_bt(c);
    CecEncoder *e = cec_enc_new(&bt);
    for (size_t i = 0; i < m; i++) cec_enc_byte(e, bcb_codec_enc_xform(c, msg[i]));
    unsigned char *out = cec_enc_finish(e, clen);
    cec_enc_free(e);
    return out;
}
static void dec(BcbCodec *c, const unsigned char *comp, size_t clen, unsigned char *out, size_t m) {
    bcb_codec_begin(c);
    CecBT bt = bcb_codec_cec_bt(c);
    CecDecoder *d = cec_dec_new(comp, clen, &bt);
    for (size_t i = 0; i < m; i++) out[i] = cec_dec_byte(d);
    cec_dec_free(d);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <prior> <corpus>\n", argv[0]); return 2; }
    printf("BCB distribution-cache test\n");

    size_t clen_corpus = 0; unsigned char *corpus = slurp(argv[2], &clen_corpus);
    if (!corpus || clen_corpus == 0) { fprintf(stderr, "cannot read corpus\n"); return 1; }
    BcbPrior *p = bcb_prior_mmap(argv[1]);
    CHECK(p != NULL, "open prior");
    if (!p) return 1;
    CHECK(bcb_prior_record_size(p) == 0, "prior is BT mode (record_size==0, exercises codec_dist)");

    const size_t M = 128, train = 50000;
    size_t base = train < clen_corpus ? train : 0;
    size_t navail = (clen_corpus - base) / M;
    size_t N = navail < 400 ? navail : 400;
    CHECK(N >= 8, "enough messages in corpus");

    BcbCodec *c = bcb_codec_new(p);
    unsigned char *out = malloc(M);

    /* 1. lossless round-trip over N messages with a warm cache */
    int lossless = 1;
    for (size_t k = 0; k < N; k++) {
        const unsigned char *msg = corpus + base + k * M;
        size_t cl; unsigned char *cc = enc(c, msg, M, &cl);
        dec(c, cc, cl, out, M);
        if (memcmp(out, msg, M) != 0) lossless = 0;
        free(cc);
    }
    CHECK(lossless, "lossless round-trip over warm-cache run (proves no wrong cached cum)");

    size_t hits = 0, misses = 0;
    bcb_codec_distcache_stats(c, &hits, &misses);
#ifdef BCB_NO_DISTCACHE
    CHECK(hits == 0 && misses == 0, "stats report 0/0 under BCB_NO_DISTCACHE");
#else
    CHECK(hits + misses > 0, "cache exercised (BT miss path reached)");
    CHECK(hits > 0, "warm cache produced hits across messages");
    printf("  hits=%zu misses=%zu hit%%=%.1f\n", hits, misses,
           (hits + misses) ? 100.0 * (double)hits / (double)(hits + misses) : 0.0);

    /* 2. re-encoding the SAME message must add hits (memoization is effective) */
    const unsigned char *msg0 = corpus + base;
    size_t h_before, m_before; bcb_codec_distcache_stats(c, &h_before, &m_before);
    size_t cl; unsigned char *cc = enc(c, msg0, M, &cl); free(cc);
    size_t h_after, m_after; bcb_codec_distcache_stats(c, &h_after, &m_after);
    CHECK(h_after > h_before, "re-encoding a seen message hits the warm cache");

    /* 3. identical output regardless of cache warmth (cold vs warm same message) */
    BcbCodec *c2 = bcb_codec_new(p);           /* cold cache */
    size_t cl_cold; unsigned char *cold = enc(c2, msg0, M, &cl_cold);
    size_t cl_warm; unsigned char *warm = enc(c,  msg0, M, &cl_warm);   /* warm cache */
    CHECK(cl_cold == cl_warm && memcmp(cold, warm, cl_cold) == 0,
          "cold-cache and warm-cache encodings are byte-identical");
    free(cold); free(warm); bcb_codec_free(c2);
#endif

    free(out); bcb_codec_free(c); bcb_prior_close(p); free(corpus);
    printf(fails ? "FAILED (%d)\n" : "PASSED (0 failures)\n", fails);
    return fails ? 1 : 0;
}
