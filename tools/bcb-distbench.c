/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* bcb-distbench.c — measure the per-instance BT distribution cache (codec_dist).
 *
 *   bcb-distbench <prior> <corpus> [--train-size N] [--msg-size M] [--msgs K]
 *                 [--reps R] [--dump FILE]
 *
 * Drives the *per-instance* codec path (bcb_codec_new → bcb_codec_cec_bt →
 * codec_dist), which is the path the public API ships and the only caller of
 * bt_v3_distribution_r. msgbench/landmark use the global one-shot path and do not
 * exercise this cache, so this tool exists to measure it honestly:
 *   • encode/decode throughput (msgs/s) at a fixed message size,
 *   • distribution-cache hit% (hits / (hits+misses)) via bcb_codec_distcache_stats,
 *   • lossless round-trip, and
 *   • --dump: the concatenated range-coder payloads, for a byte-for-byte cmp
 *     between a cache-on build and a -DBCB_NO_DISTCACHE build (bit-identical proof).
 *
 * One codec is reused across all messages (the streaming-handle pattern), with
 * bcb_codec_begin() at each message boundary — exactly how compress_core resets. */
#define _POSIX_C_SOURCE 199309L          /* clock_gettime / CLOCK_MONOTONIC under -std=c99 */
#include "ce_compress.h"
#include "btv3.h"
#include "bcb_prior.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz ? (size_t)sz : 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_len = n;
    return buf;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Encode one message through the per-instance codec (BT path; xform is identity
 * for non-structural priors). Returns malloc'd payload, *clen set. */
static unsigned char *enc_msg(BcbCodec *c, const unsigned char *msg, size_t m, size_t *clen) {
    bcb_codec_begin(c);
    CecBT bt = bcb_codec_cec_bt(c);
    CecEncoder *e = cec_enc_new(&bt);
    if (!e) return NULL;
    for (size_t i = 0; i < m; i++) cec_enc_byte(e, bcb_codec_enc_xform(c, msg[i]));
    unsigned char *out = cec_enc_finish(e, clen);   /* payload ownership → caller */
    cec_enc_free(e);
    return out;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <prior> <corpus> [--train-size N] [--msg-size M] "
                        "[--msgs K] [--reps R] [--dump FILE]\n", argv[0]);
        return 2;
    }
    const char *prior_path = argv[1], *corpus_path = argv[2], *dump_path = NULL;
    size_t train_size = 50000, msg_size = 128, want_msgs = 500;
    int reps = 50;
    for (int i = 3; i + 1 < argc; i++) {
        if      (!strcmp(argv[i], "--train-size")) train_size = (size_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--msg-size"))   msg_size   = (size_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--msgs"))       want_msgs  = (size_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--reps"))       reps       = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump"))       dump_path  = argv[++i];
    }

    size_t corpus_len = 0;
    unsigned char *corpus = slurp(corpus_path, &corpus_len);
    if (!corpus) { fprintf(stderr, "cannot read corpus %s\n", corpus_path); return 1; }
    if (train_size >= corpus_len) train_size = 0;

    size_t avail = (corpus_len - train_size) / msg_size;
    size_t nmsg = want_msgs < avail ? want_msgs : avail;
    if (nmsg == 0) { fprintf(stderr, "no messages (corpus too small)\n"); free(corpus); return 1; }

    BcbPrior *p = bcb_prior_mmap(prior_path);
    if (!p) { fprintf(stderr, "cannot open prior %s\n", prior_path); free(corpus); return 1; }
    int rec = bcb_prior_record_size(p);

    BcbCodec *c = bcb_codec_new(p);
    if (!c) { fprintf(stderr, "codec alloc failed\n"); bcb_prior_close(p); free(corpus); return 1; }

    /* ── one clean pass: round-trip + cache stats + ratio + payload capture ── */
    unsigned char **comp = (unsigned char **)malloc(nmsg * sizeof(*comp));
    size_t *clen = (size_t *)malloc(nmsg * sizeof(*clen));
    unsigned char *dec = (unsigned char *)malloc(msg_size);
    size_t total_orig = 0, total_comp = 0;
    int lossless = 1;

    size_t h0, m0; bcb_codec_distcache_stats(c, &h0, &m0);
    for (size_t k = 0; k < nmsg; k++) {
        const unsigned char *msg = corpus + train_size + k * msg_size;
        comp[k] = enc_msg(c, msg, msg_size, &clen[k]);
        total_orig += msg_size; total_comp += clen[k];
        /* round-trip */
        bcb_codec_begin(c);
        CecBT bt = bcb_codec_cec_bt(c);
        CecDecoder *d = cec_dec_new(comp[k], clen[k], &bt);
        for (size_t i = 0; i < msg_size; i++) { (void)cec_dec_byte(d); dec[i] = bcb_codec_dec_last(c); }
        cec_dec_free(d);
        /* For BT (non-structural) priors, dec_last is not used; decode the byte
         * directly. Re-run the decode capturing the coder output for that case. */
        if (rec == 0) {
            bcb_codec_begin(c);
            CecBT bt2 = bcb_codec_cec_bt(c);
            CecDecoder *d2 = cec_dec_new(comp[k], clen[k], &bt2);
            for (size_t i = 0; i < msg_size; i++) dec[i] = cec_dec_byte(d2);
            cec_dec_free(d2);
        }
        if (memcmp(dec, msg, msg_size) != 0) lossless = 0;
    }
    size_t h1, m1; bcb_codec_distcache_stats(c, &h1, &m1);
    size_t hits = h1 - h0, misses = m1 - m0;
    double hitpct = (hits + misses) ? 100.0 * (double)hits / (double)(hits + misses) : 0.0;
    double ratio  = total_comp ? (double)total_orig / (double)total_comp : 0.0;

    /* ── dump payloads (for bit-identical cmp across builds) ── */
    if (dump_path) {
        FILE *df = fopen(dump_path, "wb");
        if (df) { for (size_t k = 0; k < nmsg; k++) fwrite(comp[k], 1, clen[k], df); fclose(df); }
    }

    /* ── encode throughput ── */
    double t0 = now_s();
    for (int r = 0; r < reps; r++)
        for (size_t k = 0; k < nmsg; k++) {
            const unsigned char *msg = corpus + train_size + k * msg_size;
            size_t cl; unsigned char *cc = enc_msg(c, msg, msg_size, &cl);
            free(cc);
        }
    double enc_s = now_s() - t0;
    double enc_mps = enc_s > 0 ? (double)(nmsg * (size_t)reps) / enc_s : 0;

    /* ── decode throughput ── */
    t0 = now_s();
    for (int r = 0; r < reps; r++)
        for (size_t k = 0; k < nmsg; k++) {
            bcb_codec_begin(c);
            CecBT bt = bcb_codec_cec_bt(c);
            CecDecoder *d = cec_dec_new(comp[k], clen[k], &bt);
            for (size_t i = 0; i < msg_size; i++) (void)cec_dec_byte(d);
            cec_dec_free(d);
        }
    double dec_s = now_s() - t0;
    double dec_mps = dec_s > 0 ? (double)(nmsg * (size_t)reps) / dec_s : 0;

#ifdef BCB_NO_DISTCACHE
    const char *cache = "off";
#else
    const char *cache = "on";
#endif
    printf("distbench cache=%-3s prior=%s rec=%d msgs=%zu msg=%zuB reps=%d\n",
           cache, prior_path, rec, nmsg, msg_size, reps);
    printf("  ratio=%.3fx  enc=%.0f msgs/s  dec=%.0f msgs/s  "
           "cache:hit%%=%.1f hits=%zu misses=%zu  lossless=%s\n",
           ratio, enc_mps, dec_mps, hitpct, hits, misses, lossless ? "yes" : "NO");

    for (size_t k = 0; k < nmsg; k++) free(comp[k]);
    free(comp); free(clen); free(dec);
    bcb_codec_free(c); bcb_prior_close(p); free(corpus);
    return lossless ? 0 : 1;
}
