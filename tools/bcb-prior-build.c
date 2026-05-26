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

    int rc = (lm_k > 0) ? bcb_prior_save_with_landmarks(out_path, lm_n, lm_k)
                        : bcb_prior_save(out_path);
    if (rc != 0) {
        fprintf(stderr, "failed to write %s\n", out_path);
        free(corpus); bt_v3_free();
        return 1;
    }
    fprintf(stderr, "trained %zu bytes -> %s (%lu BT entries, landmark-k=%u N=%d)\n",
            tl, out_path, bt_v3_entries(), lm_k, lm_n);
    free(corpus);
    bt_v3_free();
    return 0;
}
