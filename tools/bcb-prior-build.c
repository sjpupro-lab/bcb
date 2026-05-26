/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* bcb-prior-build.c — 학습 코퍼스 → .bcb-prior 파일.
 *
 *   bcb-prior-build <train.txt> <out.bcb-prior> [--train-size N]
 *
 * 코퍼스(또는 그 앞 N 바이트)로 btv3 BT 를 학습한 뒤 prior 를 직렬화한다.
 * 인코더·디코더는 같은 .bcb-prior 파일을 mmap 하여 공유한다.
 */
#include "ce_compress.h"
#include "btv3.h"
#include "bcb_prior.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LM_BACKOFF_W 64.0    /* PR-1 측정과 동일한 backoff pseudo-count */

typedef struct { unsigned long idx; unsigned total; } CtxRef;
static int cmp_ctxref(const void *a, const void *b) {
    unsigned ta = ((const CtxRef *)a)->total, tb = ((const CtxRef *)b)->total;
    return ta < tb ? 1 : (ta > tb ? -1 : 0);
}

/* P[256](합 sum) 를 width[256] 정수로 양자화. 합=scale, 각 width>=1 보장. */
static void quantize(const double *P, double sum, uint16_t *w, int scale) {
    int acc = 0;
    for (int b = 0; b < 256; b++) {
        double x = sum > 0 ? P[b] / sum * scale : (double)scale / 256.0;
        int v = (int)(x + 0.5);
        if (v < 1) v = 1;
        if (v > scale) v = scale;
        w[b] = (uint16_t)v; acc += v;
    }
    int diff = scale - acc;
    while (diff > 0) {                       /* 부족분: 가장 확률 큰 bin 에 더함 */
        int bi = 0; double best = -1;
        for (int b = 0; b < 256; b++) { double pr = sum > 0 ? P[b] / sum : 1.0 / 256; if (pr > best) { best = pr; bi = b; } }
        w[bi]++; diff--;
    }
    while (diff < 0) {                        /* 초과분: 가장 큰 width(>1) 에서 뺌 */
        int bi = -1, mx = 1;
        for (int b = 0; b < 256; b++) if (w[b] > mx) { mx = w[b]; bi = b; }
        if (bi < 0) break;
        w[bi]--; diff++;
    }
}

/* 학습된 btv3 에서 길이 n 빈출 context top-k 를 골라 sharper cum(정수 width) 생성.
 * lm_ctx(k*n), lm_cum(k*256) 를 채우고 실제 k 반환. 호출 후 window 는 복원해야 함. */
static unsigned build_landmarks(int n, unsigned k, unsigned char *lm_ctx, uint16_t *lm_cum) {
    unsigned long npool = bt_v3_ctx_pool_count();
    CtxRef *refs = (CtxRef *)malloc(sizeof(CtxRef) * (npool ? npool : 1));
    unsigned long nlen = 0;
    unsigned char ctx[64]; int len; unsigned total; unsigned freq[256];
    for (unsigned long i = 0; i < npool; i++) {
        if (bt_v3_ctx_at(i, ctx, &len, &total, NULL) != 0) continue;
        if (len == n && total > 0) { refs[nlen].idx = i; refs[nlen].total = total; nlen++; }
    }
    qsort(refs, nlen, sizeof(CtxRef), cmp_ctxref);
    if (k > nlen) k = (unsigned)nlen;

    uint32_t cum[257];
    for (unsigned r = 0; r < k; r++) {
        bt_v3_ctx_at(refs[r].idx, ctx, &len, &total, freq);
        memcpy(lm_ctx + (size_t)r * n, ctx, (size_t)n);
        bt_v3_set_window(ctx, n);
        bt_v3_distribution(cum, CEC_RC_SCALE);
        double P[256], sum = 0, tot = (double)total + LM_BACKOFF_W;
        for (int b = 0; b < 256; b++) {
            double pbt = (double)(cum[b + 1] - cum[b]) / (double)CEC_RC_SCALE;
            P[b] = (double)freq[b] / tot + LM_BACKOFF_W * pbt / tot;   /* 경험적 + BT blend backoff */
            sum += P[b];
        }
        quantize(P, sum, lm_cum + (size_t)r * 256, CEC_RC_SCALE);
    }
    free(refs);
    return k;
}

static unsigned char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz ? (size_t)sz : 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_len = n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <train.txt> <out.bcb-prior> [--train-size N]\n", argv[0]);
        return 2;
    }
    const char *train_path = argv[1], *out_path = argv[2];
    size_t train_size = 0;
    int lm_n = 8; unsigned lm_k = 0;          /* landmark-k 0 = off (기존 동작) */
    for (int i = 3; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--train-size")) train_size = (size_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--landmark-n")) lm_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--landmark-k")) lm_k = (unsigned)strtoul(argv[++i], NULL, 10);
    }
    if (lm_n < 1 || lm_n > 24) { fprintf(stderr, "landmark-n must be 1..24\n"); return 2; }

    size_t corpus_len = 0;
    unsigned char *corpus = slurp(train_path, &corpus_len);
    if (!corpus) { fprintf(stderr, "cannot read %s\n", train_path); return 1; }
    size_t tl = (train_size && train_size < corpus_len) ? train_size : corpus_len;

    CecBT bt = btv3_cec_bt();                    /* init + reset */
    for (size_t i = 0; i < tl; i++) bt.train(corpus[i], bt.user);

    /* landmark 추출 (선택). window 를 건드리므로 저장 전 복원. */
    unsigned char *lm_ctx = NULL; uint16_t *lm_cum = NULL; unsigned k_used = 0;
    if (lm_k > 0) {
        const unsigned char *win; int wl = bt_v3_window(&win);
        unsigned char saved_win[32]; int saved_len = wl > 32 ? 32 : wl;
        memcpy(saved_win, win, (size_t)saved_len);

        lm_ctx = (unsigned char *)malloc((size_t)lm_k * lm_n);
        lm_cum = (uint16_t *)malloc((size_t)lm_k * 256 * sizeof(uint16_t));
        k_used = build_landmarks(lm_n, lm_k, lm_ctx, lm_cum);

        bt_v3_set_window(saved_win, saved_len);   /* 학습 종료 window 복원 */
        bcb_prior_set_landmarks(lm_n, k_used, lm_ctx, lm_cum);
    }

    if (bcb_prior_save(out_path) != 0) {
        fprintf(stderr, "failed to write %s\n", out_path);
        free(corpus); free(lm_ctx); free(lm_cum); bt_v3_free();
        return 1;
    }
    fprintf(stderr, "trained %zu bytes -> %s (%lu BT entries, %u landmarks N=%d)\n",
            tl, out_path, bt_v3_entries(), k_used, lm_n);
    free(corpus); free(lm_ctx); free(lm_cum);
    bt_v3_free();
    return 0;
}
