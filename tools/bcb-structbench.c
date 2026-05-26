/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* bcb-structbench.c — Structural (position-aware) landmark 잠재력 측정 (v6 PR-1).
 *
 *   bcb-structbench --corpus <p> --record-size R [--train-size N] [--test-size M] [--md] [--label L]
 *
 * 고정 크기 R 레코드로 정렬된 binary 데이터에서, "hit" 을 byte sequence 가 아니라
 * *레코드 내 위치(position)* 로 정의했을 때의 압축비 잠재력을 측정한다:
 *   base       — 현재 BCB(v3 BT, frozen, 순차 context).
 *   pos-byte   — 위치별 byte 경험적 분포 P(byte | position).
 *   pos-delta  — 위치별 delta 경험적 분포 P(byte - prev_record[pos] | position).
 *   hybrid     — 위치마다 train entropy 가 낮은 모드(byte/delta) 자동 선택.
 *
 * entropy 추정치(cross-entropy bits)이며 실제 코딩 바이트·무손실은 PR-2/3 에서.
 */
#define _POSIX_C_SOURCE 200809L
#include "ce_compress.h"
#include "btv3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define RMAX 256
#define SMOOTH 0.2     /* add-alpha (nonzero 보장, sharp 분포 보존) */

static unsigned char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    unsigned char *b = (unsigned char *)malloc((size_t)sz);
    size_t n = fread(b, 1, (size_t)sz, f); fclose(f);
    *out_len = n; return b;
}

int main(int argc, char **argv) {
    const char *corpus_path = NULL, *label = NULL;
    size_t train_size = 50000, test_size = 50000;
    int R = 0, md = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--corpus") && i+1<argc) corpus_path = argv[++i];
        else if (!strcmp(argv[i], "--record-size") && i+1<argc) R = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--train-size") && i+1<argc) train_size = (size_t)strtoul(argv[++i],NULL,10);
        else if (!strcmp(argv[i], "--test-size") && i+1<argc) test_size = (size_t)strtoul(argv[++i],NULL,10);
        else if (!strcmp(argv[i], "--label") && i+1<argc) label = argv[++i];
        else if (!strcmp(argv[i], "--md")) md = 1;
    }
    if (!corpus_path || R < 1 || R > RMAX) {
        fprintf(stderr, "usage: %s --corpus <p> --record-size R(1..256) [--train-size N] [--test-size M] [--md] [--label L]\n", argv[0]);
        return 2;
    }
    size_t clen = 0; unsigned char *corpus = slurp(corpus_path, &clen);
    if (!corpus) { fprintf(stderr, "cannot read %s\n", corpus_path); return 1; }
    train_size -= train_size % R;                 /* 레코드 정렬 */
    if (train_size == 0 || train_size + (size_t)R > clen) { fprintf(stderr, "corpus too small\n"); return 1; }
    const unsigned char *train = corpus, *test = corpus + train_size;
    size_t test_len = clen - train_size;
    if (test_size < test_len) test_len = test_size;
    test_len -= test_len % R;

    /* 위치별 byte / delta 경험적 빈도 (train) */
    static double bf[RMAX][256], df[RMAX][256];
    memset(bf, 0, sizeof bf); memset(df, 0, sizeof df);
    size_t nrec_tr = train_size / R;
    for (size_t r = 0; r < nrec_tr; r++)
        for (int p = 0; p < R; p++) {
            unsigned char b = train[r*R + p];
            bf[p][b] += 1.0;
            if (r > 0) { unsigned char d = (unsigned char)(b - train[(r-1)*R + p]); df[p][d] += 1.0; }
        }

    /* 위치별 mode 결정: train self cross-entropy 비교 (byte vs delta) */
    int mode_delta[RMAX];          /* 1=delta, 0=byte */
    for (int p = 0; p < R; p++) {
        double tb = 0, td = 0; double totb = 0, totd = 0;
        for (int b = 0; b < 256; b++) { totb += bf[p][b]; totd += df[p][b]; }
        for (size_t r = 0; r < nrec_tr; r++) {
            unsigned char b = train[r*R + p];
            double pb = (bf[p][b] + SMOOTH) / (totb + 256*SMOOTH);
            tb += -log2(pb);
            if (r > 0) { unsigned char d=(unsigned char)(b-train[(r-1)*R+p]); double pd=(df[p][d]+SMOOTH)/(totd+256*SMOOTH); td += -log2(pd); }
            else td += 8.0;
        }
        mode_delta[p] = (td < tb) ? 1 : 0;
    }

    /* base: trained btv3 를 test 위에서 순차 처리 (frozen) */
    CecBT bt = btv3_cec_bt();
    for (size_t i = 0; i < train_size; i++) bt.train(train[i], bt.user);
    bt_v3_freeze(1);
    bt_v3_reset_window();

    double base_bits = 0, pb_bits = 0, pd_bits = 0, hy_bits = 0;
    uint32_t cum[257];
    unsigned char win[24]; int wl = 0;
    for (size_t j = 0; j < test_len; j++) {
        unsigned char b = test[j];
        int p = (int)(j % R);
        /* base */
        bt_v3_set_window(win, wl);
        bt_v3_distribution(cum, CEC_RC_SCALE);
        double pbase = (double)(cum[b+1]-cum[b])/(double)CEC_RC_SCALE; if (pbase<=0) pbase=1.0/CEC_RC_SCALE;
        base_bits += -log2(pbase);
        /* pos-byte */
        double totb=0; for (int x=0;x<256;x++) totb+=bf[p][x];
        double pby = (bf[p][b]+SMOOTH)/(totb+256*SMOOTH);
        pb_bits += -log2(pby);
        /* pos-delta */
        double pdl;
        if (j >= (size_t)R) {
            unsigned char d=(unsigned char)(b - test[j-R]);
            double totd=0; for (int x=0;x<256;x++) totd+=df[p][x];
            pdl = (df[p][d]+SMOOTH)/(totd+256*SMOOTH);
        } else pdl = 1.0/256;
        pd_bits += -log2(pdl);
        /* hybrid */
        hy_bits += (mode_delta[p] && j >= (size_t)R) ? -log2(pdl) : -log2(pby);
        /* advance base window */
        if (wl < 24) win[wl++] = b; else { memmove(win, win+1, 23); win[23] = b; }
    }
    bt_v3_free();

    double n = (double)test_len;
    double bx = 8.0/(base_bits/n), pbx = 8.0/(pb_bits/n), pdx = 8.0/(pd_bits/n), hyx = 8.0/(hy_bits/n);
    int ndelta = 0; for (int p=0;p<R;p++) ndelta += mode_delta[p];
    double gain = base_bits>0 ? 100.0*(base_bits-hy_bits)/base_bits : 0;

    if (md) {
        if (label) printf("### %s (R=%d)\n\n", label, R);
        printf("| metric | base | pos-byte | pos-delta | hybrid |\n|---|---|---|---|---|\n");
        printf("| ratio | %.2f | %.2f | %.2f | **%.2f** |\n", bx, pbx, pdx, hyx);
        printf("| bits/byte | %.3f | %.3f | %.3f | %.3f |\n", base_bits/n, pb_bits/n, pd_bits/n, hy_bits/n);
        printf("\nhybrid gain vs base: **%+.1f%%**, delta-mode positions: %d/%d\n\n", gain, ndelta, R);
    } else {
        printf("%-16s R=%d test=%zuB  base=%.2fx  pos-byte=%.2fx  pos-delta=%.2fx  hybrid=%.2fx  (gain %+.1f%%, delta-pos %d/%d)\n",
               label?label:corpus_path, R, test_len, bx, pbx, pdx, hyx, gain, ndelta, R);
    }
    free(corpus);
    return 0;
}
