/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* bcb-realbench.c — real-data benchmark with *message boundaries*.
 *
 * msgbench 는 임의 고정폭 윈도를 잘라 측정한다(합성 코퍼스에 적합). realbench 는
 * 실제 메시지/레코드 경계를 존중해, 실데이터에서 "공유 prior" 압축을 공정 비교한다.
 *
 * 입력:
 *   --corpus <bytes>     : 메시지들을 이어붙인 바이너리
 *   --index  <ints>      : 메시지별 길이(줄당 1개 정수). 합 == corpus 크기.
 *                          (--record-size 를 주면 index 없이 고정 레코드로 분할)
 *   --train-msgs N       : 앞 N개 메시지 = 공유 prior/dict. 나머지 = 테스트.
 *   --mode landmark|structural
 *   --record-size R      : structural 모드(고정 레코드) / index 없을 때 분할 단위
 *   --landmark-k K, --landmark-n n : landmark 파라미터(기본 256/8)
 *   [--md] [--label L]
 *
 * 비교 대상 (모두 같은 train 으로 공유 컨텍스트 부여, 메시지 독립 압축, round-trip 검증):
 *   BCB(+lm 또는 +structural) · brotli+dict · zstd+dict · gzip(무사전) · zlib+dict
 * 측정은 각 압축기의 코어 출력 바이트(외부 길이 framing 제외; msgbench 와 동일 기준).
 */
#define _POSIX_C_SOURCE 200809L
#include "ce_compress.h"
#include "btv3.h"
#include "bcb_prior.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <brotli/encode.h>
#include <brotli/decode.h>
#include <brotli/shared_dictionary.h>
#include <zstd.h>
#include <zlib.h>

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    *out_len = n; return buf;
}

/* ── BCB: per-instance codec (landmark or structural prior). core payload bytes. ── */
static size_t bcb_enc(BcbPrior *p, const uint8_t *msg, size_t mlen, int *lossless) {
    BcbCodec *c = bcb_codec_new(p);
    if (!c) { *lossless = 0; return 0; }
    bcb_codec_begin(c);
    CecBT bt = bcb_codec_cec_bt(c);
    CecEncoder *e = cec_enc_new(&bt);
    for (size_t i = 0; i < mlen; i++) cec_enc_byte(e, msg[i]);
    size_t clen = 0; uint8_t *comp = cec_enc_finish(e, &clen); cec_enc_free(e);
    /* decode with a fresh codec state (mirror real decoder) */
    bcb_codec_begin(c);
    CecBT bt2 = bcb_codec_cec_bt(c);
    uint8_t *dec = cec_decompress(comp, clen, mlen, &bt2);
    *lossless = (mlen == 0) ? 1 : (dec && memcmp(dec, msg, mlen) == 0);
    free(dec); free(comp); bcb_codec_free(c);
    return clen;
}

/* ── brotli + raw shared dictionary (quality 11), round-trip verified. ── */
static size_t brotli_enc(const BrotliEncoderPreparedDictionary *dict,
                         const uint8_t *train, size_t train_len,
                         const uint8_t *msg, size_t mlen, int *lossless) {
    size_t cap = BrotliEncoderMaxCompressedSize(mlen); if (cap < 64) cap = 64;
    uint8_t *out = (uint8_t *)malloc(cap);
    BrotliEncoderState *s = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    BrotliEncoderSetParameter(s, BROTLI_PARAM_QUALITY, BROTLI_MAX_QUALITY);
    BrotliEncoderSetParameter(s, BROTLI_PARAM_LGWIN, BROTLI_MAX_WINDOW_BITS);
    BrotliEncoderSetParameter(s, BROTLI_PARAM_SIZE_HINT, (uint32_t)mlen);
    if (dict) BrotliEncoderAttachPreparedDictionary(s, dict);
    size_t avail_in = mlen, avail_out = cap, total = 0;
    const uint8_t *ni = msg; uint8_t *no = out;
    BrotliEncoderCompressStream(s, BROTLI_OPERATION_FINISH, &avail_in, &ni, &avail_out, &no, &total);
    size_t produced = cap - avail_out;
    BrotliEncoderDestroyInstance(s);
    /* verify */
    uint8_t *dec = (uint8_t *)malloc(mlen ? mlen : 1); size_t dlen = mlen;
    BrotliDecoderState *ds = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (train_len)
        BrotliDecoderAttachDictionary(ds, BROTLI_SHARED_DICTIONARY_RAW, train_len, train);
    size_t ai = produced, ao = dlen; const uint8_t *di = out; uint8_t *dout = dec; size_t tot = 0;
    BrotliDecoderResult r = BrotliDecoderDecompressStream(ds, &ai, &di, &ao, &dout, &tot);
    size_t got = dlen - ao;
    *lossless = (r == BROTLI_DECODER_RESULT_SUCCESS && got == mlen && (mlen == 0 || memcmp(dec, msg, mlen) == 0));
    BrotliDecoderDestroyInstance(ds);
    free(out); free(dec);
    return produced;
}

/* ── zstd + raw content dictionary (max level, minimal frame), verified. ── */
static size_t zstd_enc(ZSTD_CCtx *zc, ZSTD_DCtx *zd, const uint8_t *train, size_t train_len,
                       const uint8_t *msg, size_t mlen, int *lossless) {
    size_t cap = ZSTD_compressBound(mlen); uint8_t *out = (uint8_t *)malloc(cap);
    size_t r = ZSTD_compress2(zc, out, cap, msg, mlen);
    if (ZSTD_isError(r)) { *lossless = 0; free(out); return 0; }
    uint8_t *dec = (uint8_t *)malloc(mlen ? mlen : 1);
    ZSTD_DCtx_reset(zd, ZSTD_reset_session_only);
    ZSTD_DCtx_loadDictionary(zd, train, train_len);
    size_t dr = ZSTD_decompressDCtx(zd, dec, mlen, out, r);
    *lossless = (!ZSTD_isError(dr) && dr == mlen && (mlen == 0 || memcmp(dec, msg, mlen) == 0));
    free(out); free(dec);
    return r;
}

/* ── raw deflate. dict!=NULL → zlib+dict (deflateSetDictionary, last 32KB). ── */
static size_t deflate_enc(const uint8_t *dict, size_t dict_len, int gzip_format,
                          const uint8_t *msg, size_t mlen, int *lossless) {
    /* zlib dictionary window is 32KB; use the tail. raw deflate(-15) for dict,
     * gzip(15|16) for the no-dict "gzip" baseline. */
    z_stream zs; memset(&zs, 0, sizeof zs);
    int wbits = gzip_format ? (15 | 16) : -15;
    if (deflateInit2(&zs, 9, Z_DEFLATED, wbits, 9, Z_DEFAULT_STRATEGY) != Z_OK) { *lossless = 0; return 0; }
    if (dict) {
        size_t dl = dict_len, off = 0;
        if (dl > 32768) { off = dl - 32768; dl = 32768; }
        deflateSetDictionary(&zs, dict + off, (uInt)dl);
    }
    uLong cap = deflateBound(&zs, (uLong)mlen) + 32;
    uint8_t *out = (uint8_t *)malloc(cap);
    zs.next_in = (Bytef *)msg; zs.avail_in = (uInt)mlen;
    zs.next_out = out; zs.avail_out = (uInt)cap;
    deflate(&zs, Z_FINISH);
    size_t produced = cap - zs.avail_out;
    deflateEnd(&zs);
    /* verify */
    z_stream iz; memset(&iz, 0, sizeof iz);
    inflateInit2(&iz, wbits);
    /* raw deflate streams (windowBits<0) do not emit Z_NEED_DICT; the dictionary
     * must be installed up front. zlib-wrapped streams signal via Z_NEED_DICT. */
    if (dict && !gzip_format) {
        size_t dl = dict_len, o = 0; if (dl > 32768) { o = dl - 32768; dl = 32768; }
        inflateSetDictionary(&iz, dict + o, (uInt)dl);
    }
    uint8_t *dec = (uint8_t *)malloc(mlen ? mlen : 1);
    iz.next_in = out; iz.avail_in = (uInt)produced;
    iz.next_out = dec; iz.avail_out = (uInt)(mlen ? mlen : 1);
    int ir = inflate(&iz, Z_FINISH);
    if (ir == Z_NEED_DICT && dict) {
        size_t dl = dict_len, o = 0; if (dl > 32768) { o = dl - 32768; dl = 32768; }
        inflateSetDictionary(&iz, dict + o, (uInt)dl);
        ir = inflate(&iz, Z_FINISH);
    }
    size_t got = (mlen ? mlen : 1) - iz.avail_out;
    *lossless = ((ir == Z_STREAM_END || ir == Z_OK) && got == mlen && (mlen == 0 || memcmp(dec, msg, mlen) == 0));
    inflateEnd(&iz); free(out); free(dec);
    return produced;
}

int main(int argc, char **argv) {
    const char *corpus_path = NULL, *index_path = NULL, *label = NULL, *mode = "landmark";
    size_t train_msgs = 0, max_test = 0; int R = 0, lm_k = 256, lm_n = 8, md = 0, rpm = 1;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--corpus") && i+1<argc) corpus_path = argv[++i];
        else if (!strcmp(argv[i], "--index") && i+1<argc) index_path = argv[++i];
        else if (!strcmp(argv[i], "--train-msgs") && i+1<argc) train_msgs = (size_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--max-test") && i+1<argc) max_test = (size_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--mode") && i+1<argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--record-size") && i+1<argc) R = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--recs-per-msg") && i+1<argc) rpm = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--landmark-k") && i+1<argc) lm_k = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--landmark-n") && i+1<argc) lm_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--label") && i+1<argc) label = argv[++i];
        else if (!strcmp(argv[i], "--md")) md = 1;
    }
    if (!corpus_path) { fprintf(stderr, "usage: %s --corpus <p> (--index <p> | --record-size R) --train-msgs N --mode landmark|structural [--label L] [--md]\n", argv[0]); return 2; }
    int structural = !strcmp(mode, "structural");

    size_t clen = 0; uint8_t *corpus = slurp(corpus_path, &clen);
    if (!corpus) { fprintf(stderr, "cannot read %s\n", corpus_path); return 1; }

    /* message offsets/lengths */
    size_t *off = NULL, *len = NULL, nmsg = 0, cap = 0;
    if (index_path) {
        FILE *f = fopen(index_path, "r"); if (!f) { fprintf(stderr, "cannot read %s\n", index_path); return 1; }
        size_t pos = 0; long v;
        while (fscanf(f, "%ld", &v) == 1) {
            if (v < 0 || pos + (size_t)v > clen) break;
            if (nmsg == cap) { cap = cap ? cap*2 : 1024; off = realloc(off, cap*sizeof*off); len = realloc(len, cap*sizeof*len); }
            off[nmsg] = pos; len[nmsg] = (size_t)v; pos += (size_t)v; nmsg++;
        }
        fclose(f);
    } else {
        if (R < 1) { fprintf(stderr, "need --index or --record-size\n"); return 2; }
        if (rpm < 1) rpm = 1;
        size_t stride = (size_t)R * (size_t)rpm;   /* message = block of rpm records */
        nmsg = clen / stride;
        off = malloc(nmsg*sizeof*off); len = malloc(nmsg*sizeof*len);
        for (size_t i = 0; i < nmsg; i++) { off[i] = i*stride; len[i] = stride; }
    }
    if (!off || !len || nmsg == 0) { fprintf(stderr, "no messages parsed from corpus/index\n"); return 1; }
    if (train_msgs == 0 || train_msgs >= nmsg) { fprintf(stderr, "train-msgs(%zu) must be < nmsg(%zu)\n", train_msgs, nmsg); return 1; }

    size_t train_len = off[train_msgs];   /* train = first train_msgs messages, contiguous */
    const uint8_t *train = corpus;

    /* Build BCB prior from the train split. */
    BcbPrior *p = NULL; const char *pp = "/tmp/bcb_realbench.bcb-prior";
    if (structural && R < 1) { fprintf(stderr, "structural mode needs --record-size\n"); return 2; }
    /* The prior file always carries a trained BT section (serialized by
     * bt_v3_export), so btv3 must be initialised+trained even in schema mode —
     * same order as tools/bcb-prior-build.c. */
    CecBT lb = btv3_cec_bt();
    for (size_t i = 0; i < train_len; i++) lb.train(train[i], lb.user);
    int saved = structural ? bcb_prior_save_with_schema(pp, train, train_len, R)
                           : bcb_prior_save_with_landmarks(pp, lm_n, (unsigned)lm_k);
    bt_v3_free();
    if (saved == 0) p = bcb_prior_mmap(pp);
    if (!p) { fprintf(stderr, "failed to build BCB prior\n"); return 1; }

    /* shared dicts for the external codecs */
    BrotliEncoderPreparedDictionary *bdict =
        BrotliEncoderPrepareDictionary(BROTLI_SHARED_DICTIONARY_RAW, train_len, train, BROTLI_MAX_QUALITY, NULL, NULL, NULL);
    ZSTD_CCtx *zc = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, ZSTD_maxCLevel());
    ZSTD_CCtx_setParameter(zc, ZSTD_c_contentSizeFlag, 0);
    ZSTD_CCtx_setParameter(zc, ZSTD_c_checksumFlag, 0);
    ZSTD_CCtx_setParameter(zc, ZSTD_c_dictIDFlag, 0);
    ZSTD_CCtx_loadDictionary(zc, train, train_len);
    ZSTD_DCtx *zd = ZSTD_createDCtx();

    /* measure over the test messages */
    size_t orig = 0, bcb = 0, br = 0, zs = 0, gz = 0, zl = 0;
    size_t ntest = 0, bcb_win = 0, bcb_loss_vs_best = 0;
    int all_ll = 1, bcb_ll = 1, br_ll = 1, zs_ll = 1, gz_ll = 1, zl_ll = 1;
    for (size_t i = train_msgs; i < nmsg; i++) {
        if (max_test && ntest >= max_test) break;
        const uint8_t *m = corpus + off[i]; size_t ml = len[i];
        if (ml == 0) continue;
        int l1=1,l2=1,l3=1,l4=1,l5=1;
        size_t a = bcb_enc(p, m, ml, &l1);
        size_t b = brotli_enc(bdict, train, train_len, m, ml, &l2);
        size_t c = zstd_enc(zc, zd, train, train_len, m, ml, &l3);
        size_t d = deflate_enc(NULL, 0, 1, m, ml, &l4);          /* gzip, no dict */
        size_t e = deflate_enc(train, train_len, 0, m, ml, &l5); /* zlib + dict   */
        bcb_ll&=l1; br_ll&=l2; zs_ll&=l3; gz_ll&=l4; zl_ll&=l5; all_ll &= (l1&l2&l3&l4&l5);
        orig+=ml; bcb+=a; br+=b; zs+=c; gz+=d; zl+=e; ntest++;
        size_t best_other = b; if (c<best_other) best_other=c; if (e<best_other) best_other=e; /* dict-capable rivals */
        if (a < best_other) bcb_win++; else bcb_loss_vs_best++;
    }
    if (ntest == 0) { fprintf(stderr, "no test messages\n"); return 1; }

    double R_bcb=(double)orig/bcb, R_br=(double)orig/br, R_zs=(double)orig/zs, R_gz=(double)orig/gz, R_zl=(double)orig/zl;
    const char *bcb_label = structural ? "BCB+struct" : "BCB+lm";
    if (md) {
        if (label) printf("### %s\n\n", label);
        printf("| codec | total bytes | ratio | lossless |\n|---|---|---|---|\n");
        printf("| %s | %zu | **%.3f** | %s |\n", bcb_label, bcb, R_bcb, bcb_ll?"yes":"**NO**");
        printf("| brotli+dict | %zu | %.3f | %s |\n", br, R_br, br_ll?"yes":"**NO**");
        printf("| zstd+dict | %zu | %.3f | %s |\n", zs, R_zs, zs_ll?"yes":"**NO**");
        printf("| zlib+dict | %zu | %.3f | %s |\n", zl, R_zl, zl_ll?"yes":"**NO**");
        printf("| gzip (no dict) | %zu | %.3f | %s |\n", gz, R_gz, gz_ll?"yes":"**NO**");
        printf("\ntest msgs: %zu, orig: %zu B, train msgs: %zu (%zu B). "
               "%s wins (vs best dict rival) %zu/%zu.\n\n",
               ntest, orig, train_msgs, train_len, bcb_label, bcb_win, ntest);
    } else {
        printf("%s  train=%zu msgs/%zuB  test=%zu msgs/%zuB  mode=%s%s\n",
               label?label:corpus_path, train_msgs, train_len, ntest, orig, mode, all_ll?"":"  [LOSSLESS FAIL]");
        printf("  %-12s %12s  %7s  %s\n", "codec", "bytes", "ratio", "lossless");
        printf("  %-12s %12zu  %7.3f  %s\n", bcb_label, bcb, R_bcb, bcb_ll?"yes":"NO");
        printf("  %-12s %12zu  %7.3f  %s\n", "brotli+dict", br, R_br, br_ll?"yes":"NO");
        printf("  %-12s %12zu  %7.3f  %s\n", "zstd+dict", zs, R_zs, zs_ll?"yes":"NO");
        printf("  %-12s %12zu  %7.3f  %s\n", "zlib+dict", zl, R_zl, zl_ll?"yes":"NO");
        printf("  %-12s %12zu  %7.3f  %s\n", "gzip", gz, R_gz, gz_ll?"yes":"NO");
        printf("  %s wins (vs best dict rival): %zu/%zu, loses: %zu/%zu\n",
               bcb_label, bcb_win, ntest, bcb_loss_vs_best, ntest);
    }

    BrotliEncoderDestroyPreparedDictionary(bdict);
    ZSTD_freeCCtx(zc); ZSTD_freeDCtx(zd);
    bcb_prior_close(p); free(corpus); free(off); free(len);
    return all_ll ? 0 : 3;
}
