/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* bcb-msgbench.c — small-message benchmark: BCB vs brotli+dict vs zstd+dict.
 *
 * 다양한 메시지 크기에서 "공유 prior" 압축을 측정한다.
 *   - 코퍼스 앞 train_size 바이트 = 공유 prior(BCB 학습 / brotli·zstd 사전).
 *   - 나머지에서 message_size 바이트 윈도를 잘라 표본 메시지로 압축.
 *   - 세 압축기 모두 같은 prior/dict 를 받는 공정 비교.
 *
 * BCB 는 v3 정수 BT (lossless, ~28x 빠른 hot path) 를 쓰며 메시지마다
 * round-trip 무손실을 검증한다.
 *
 * 측정 대상은 각 압축기의 "코어 코덱 출력" 바이트다 (외부 길이 framing 제외):
 *   - BCB : range-coder payload (BCB1 컨테이너 12B 제외)
 *   - brotli : 스트림 출력 (원본 길이 미저장)
 *   - zstd : frame 출력, contentSize/checksum/dictID 비활성(최소 overhead)
 *
 * 사용법:
 *   bcb-msgbench --corpus <path> --train-size 50000 \
 *                --message-sizes 64,128,256,512,1024,2048,4096 \
 *                [--samples N] [--md] [--label NAME]
 */
#define _POSIX_C_SOURCE 200809L   /* strdup (POSIX, not C99) */
#include "ce_compress.h"
#include "btv3.h"
#include "bcb_prior.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <brotli/encode.h>
#include <brotli/shared_dictionary.h>
#include <zstd.h>

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_len = n;
    return buf;
}

/* ── BCB (v3 정수 BT). prior 학습 후 메시지 인코드 + round-trip 검증. ── */
static size_t bcb_compress(const uint8_t *train, size_t train_len,
                           const uint8_t *msg, size_t msg_len, int *lossless) {
    CecBT bt = btv3_cec_bt();                 /* init + reset global */
    for (size_t i = 0; i < train_len; i++) bt.train(train[i], bt.user);
    CecEncoder *e = cec_enc_new(&bt);
    for (size_t i = 0; i < msg_len; i++) cec_enc_byte(e, msg[i]);
    size_t clen = 0;
    uint8_t *comp = cec_enc_finish(e, &clen);
    cec_enc_free(e);

    CecBT bt2 = btv3_cec_bt();                /* reset, re-prime identical prior */
    for (size_t i = 0; i < train_len; i++) bt2.train(train[i], bt2.user);
    uint8_t *dec = cec_decompress(comp, clen, msg_len, &bt2);
    *lossless = (dec && memcmp(dec, msg, msg_len) == 0);
    free(dec);
    free(comp);
    return clen;
}

/* ── BCB + landmark prior (frozen, stateless per message) ── */
static size_t bcb_landmark_compress(BcbPrior *lmp, const uint8_t *msg, size_t msg_len, int *lossless) {
    CecBT bt = bcb_prior_cec_bt(lmp);
    bt_v3_reset_window();
    CecEncoder *e = cec_enc_new(&bt);
    for (size_t i = 0; i < msg_len; i++) cec_enc_byte(e, msg[i]);
    size_t clen = 0;
    uint8_t *comp = cec_enc_finish(e, &clen);
    cec_enc_free(e);
    bcb_prior_attach(lmp); bt_v3_reset_window();
    uint8_t *dec = cec_decompress(comp, clen, msg_len, &bt);
    *lossless = (dec && memcmp(dec, msg, msg_len) == 0);
    free(dec); free(comp);
    return clen;
}

/* ── brotli + raw shared dictionary (quality 11). ── */
static size_t brotli_compress(const BrotliEncoderPreparedDictionary *dict,
                              const uint8_t *msg, size_t msg_len) {
    size_t cap = BrotliEncoderMaxCompressedSize(msg_len);
    if (cap < 64) cap = 64;
    uint8_t *out = (uint8_t *)malloc(cap);
    BrotliEncoderState *s = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    BrotliEncoderSetParameter(s, BROTLI_PARAM_QUALITY, BROTLI_MAX_QUALITY);
    BrotliEncoderSetParameter(s, BROTLI_PARAM_LGWIN, BROTLI_MAX_WINDOW_BITS);
    BrotliEncoderSetParameter(s, BROTLI_PARAM_SIZE_HINT, (uint32_t)msg_len);
    if (dict) BrotliEncoderAttachPreparedDictionary(s, dict);

    size_t avail_in = msg_len, avail_out = cap, total_out = 0;
    const uint8_t *next_in = msg;
    uint8_t *next_out = out;
    BrotliEncoderCompressStream(s, BROTLI_OPERATION_FINISH,
                                &avail_in, &next_in, &avail_out, &next_out, &total_out);
    size_t produced = cap - avail_out;
    BrotliEncoderDestroyInstance(s);
    free(out);
    return produced;
}

/* ── zstd + raw content dictionary (max level, minimal frame). ── */
static size_t zstd_compress(ZSTD_CCtx *c, const uint8_t *msg, size_t msg_len) {
    size_t cap = ZSTD_compressBound(msg_len);
    uint8_t *out = (uint8_t *)malloc(cap);
    size_t r = ZSTD_compress2(c, out, cap, msg, msg_len);
    size_t produced = ZSTD_isError(r) ? 0 : r;
    free(out);
    return produced;
}

static void parse_sizes(const char *s, size_t *sizes, int *n, int max_n) {
    *n = 0;
    char *copy = strdup(s);
    for (char *tok = strtok(copy, ","); tok && *n < max_n; tok = strtok(NULL, ","))
        sizes[(*n)++] = (size_t)strtoul(tok, NULL, 10);
    free(copy);
}

int main(int argc, char **argv) {
    const char *corpus_path = NULL, *label = NULL;
    size_t train_size = 50000;
    int max_samples = 64;
    int md = 0;
    int lm_k = 0, lm_n = 8;            /* landmark: k>0 이면 BCB+lm 열 추가 */
    const char *sizes_arg = "64,128,256,512,1024,2048,4096";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus_path = argv[++i];
        else if (!strcmp(argv[i], "--train-size") && i + 1 < argc) train_size = (size_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--message-sizes") && i + 1 < argc) sizes_arg = argv[++i];
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) max_samples = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "--landmark-k") && i + 1 < argc) lm_k = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--landmark-n") && i + 1 < argc) lm_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--md")) md = 1;
    }
    if (!corpus_path) {
        fprintf(stderr, "usage: %s --corpus <path> [--train-size N] "
                "[--message-sizes a,b,c] [--samples N] [--md] [--label NAME]\n", argv[0]);
        return 2;
    }

    size_t corpus_len = 0;
    uint8_t *corpus = slurp(corpus_path, &corpus_len);
    if (!corpus) { fprintf(stderr, "cannot read %s\n", corpus_path); return 1; }
    if (train_size >= corpus_len) {
        fprintf(stderr, "corpus (%zu B) too small for train-size %zu\n", corpus_len, train_size);
        return 1;
    }
    const uint8_t *train = corpus;
    const uint8_t *test = corpus + train_size;
    size_t test_len = corpus_len - train_size;

    size_t sizes[32];
    int nsizes = 0;
    parse_sizes(sizes_arg, sizes, &nsizes, 32);

    /* 공유 dict/prior 를 한 번만 준비해 모든 메시지에 재사용. */
    BrotliEncoderPreparedDictionary *bdict =
        BrotliEncoderPrepareDictionary(BROTLI_SHARED_DICTIONARY_RAW, train_size, train,
                                       BROTLI_MAX_QUALITY, NULL, NULL, NULL);
    ZSTD_CCtx *zc = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, ZSTD_maxCLevel());
    ZSTD_CCtx_setParameter(zc, ZSTD_c_contentSizeFlag, 0);
    ZSTD_CCtx_setParameter(zc, ZSTD_c_checksumFlag, 0);
    ZSTD_CCtx_setParameter(zc, ZSTD_c_dictIDFlag, 0);
    ZSTD_CCtx_loadDictionary(zc, train, train_size);  /* raw content dict, sticky */

    /* landmark prior (선택): train 구간으로 한 번 빌드 → mmap 후 frozen 사용 */
    BcbPrior *lmp = NULL;
    const char *lm_path = "/tmp/bcb_msgbench.bcb-prior";
    if (lm_k > 0) {
        CecBT lb = btv3_cec_bt();
        for (size_t i = 0; i < train_size; i++) lb.train(train[i], lb.user);
        if (bcb_prior_save_with_landmarks(lm_path, lm_n, (unsigned)lm_k) == 0) {
            bt_v3_free();
            lmp = bcb_prior_mmap(lm_path);
        }
        if (!lmp) fprintf(stderr, "warning: landmark prior unavailable; skipping BCB+lm\n");
    }

    if (md) {
        if (label) printf("### %s\n\n", label);
        if (lmp) {
            printf("| msg_size | BCB(x) | BCB+lm(x) | brotli(x) | zstd(x) | winner |\n");
            printf("|---|---|---|---|---|---|\n");
        } else {
            printf("| msg_size | BCB(B) | BCB(x) | brotli(B) | brotli(x) | zstd(B) | zstd(x) | winner |\n");
            printf("|---|---|---|---|---|---|---|---|\n");
        }
    } else {
        printf("corpus: %s (%zu B), train-size: %zu, samples<=%d%s\n",
               corpus_path, corpus_len, train_size, max_samples, lmp ? ", +landmark" : "");
        if (lmp)
            printf("%-9s %8s %10s %10s %8s  %s\n",
                   "msg_size", "BCB(x)", "BCB+lm(x)", "brotli(x)", "zstd(x)", "winner");
        else
            printf("%-9s %8s %8s %10s %10s %8s %8s  %s\n",
                   "msg_size", "BCB(B)", "BCB(x)", "brotli(B)", "brotli(x)",
                   "zstd(B)", "zstd(x)", "winner");
    }

    int all_lossless = 1;
    for (int si = 0; si < nsizes; si++) {
        size_t ms = sizes[si];
        if (ms == 0 || ms > test_len) continue;
        int n = (int)(test_len / ms);
        if (n > max_samples) n = max_samples;
        if (n < 1) continue;

        double bcb_t = 0, br_t = 0, zs_t = 0, orig_t = 0;
        for (int k = 0; k < n; k++) {
            const uint8_t *msg = test + (size_t)k * ms;
            int ll = 0;
            bcb_t += (double)bcb_compress(train, train_size, msg, ms, &ll);
            if (!ll) all_lossless = 0;
            br_t += (double)brotli_compress(bdict, msg, ms);
            zs_t += (double)zstd_compress(zc, msg, ms);
            orig_t += (double)ms;
        }
        /* landmark 측정은 별도 sub-loop (btv3 global 충돌 회피: base pool 해제 후 attach) */
        double lm_t = 0;
        if (lmp) {
            bt_v3_free();
            for (int k = 0; k < n; k++) {
                int ll = 0;
                lm_t += (double)bcb_landmark_compress(lmp, test + (size_t)k * ms, ms, &ll);
                if (!ll) all_lossless = 0;
            }
        }
        double bcb_avg = bcb_t / n, br_avg = br_t / n, zs_avg = zs_t / n;
        double bcb_x = orig_t / bcb_t, br_x = orig_t / br_t, zs_x = orig_t / zs_t;
        double lm_x = lm_t > 0 ? orig_t / lm_t : 0;
        double best_bcb = (lmp && lm_x > bcb_x) ? lm_x : bcb_x;
        const char *winner = (best_bcb >= br_x && best_bcb >= zs_x)
                                 ? (lmp && lm_x >= bcb_x ? "BCB+lm" : "BCB")
                                 : (br_x >= zs_x ? "brotli" : "zstd");
        if (lmp) {
            if (md)
                printf("| %zu | %.2f | %.2f | %.2f | %.2f | %s |\n",
                       ms, bcb_x, lm_x, br_x, zs_x, winner);
            else
                printf("%-9zu %8.2f %10.2f %10.2f %8.2f  %s\n",
                       ms, bcb_x, lm_x, br_x, zs_x, winner);
        } else if (md) {
            printf("| %zu | %.0f | %.2f | %.0f | %.2f | %.0f | %.2f | %s |\n",
                   ms, bcb_avg, bcb_x, br_avg, br_x, zs_avg, zs_x, winner);
        } else {
            printf("%-9zu %8.0f %8.2f %10.0f %10.2f %8.0f %8.2f  %s\n",
                   ms, bcb_avg, bcb_x, br_avg, br_x, zs_avg, zs_x, winner);
        }
    }

    if (!md)
        printf("\nlossless (BCB round-trip, all messages): %s\n", all_lossless ? "yes" : "NO");
    if (lmp) bcb_prior_close(lmp);

    BrotliEncoderDestroyPreparedDictionary(bdict);
    ZSTD_freeCCtx(zc);
    bt_v3_free();
    free(corpus);
    return all_lossless ? 0 : 3;
}
