/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* test_v3_pool.c — BT_POOL 크기별 대규모 학습 측정
 *
 * 컴파일된 pool 크기(BT_POOL = 2^BCB_POOL_BITS)에서 large 코퍼스를 학습량을 키우며:
 *   entries, pool 포화 여부, 4KB 발췌 압축비, 무손실, footprint 를 측정.
 * 8M(기본) / 32M(-DBCB_POOL_BITS=25) / 64M(=26) 바이너리를 비교한다.
 *
 * 코퍼스: tests/corpus/large.txt (없으면 fetch_large.sh 안내 후 종료).
 */
#include "ce_compress.h"
#include "btv3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EX_LEN 4096

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t*)malloc((size_t)sz);
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    *out_len = n; return buf;
}

int main(int argc, char **argv) {
    const char *path = "tests/corpus/large.txt";
    size_t len = 0; uint8_t *c = slurp(path, &len);
    if (!c) { printf("[skip] %s 없음 — 'sh tests/corpus/fetch_large.sh' 로 생성\n", path); return 0; }

    BtV3Mem m0; bt_v3_footprint(&m0);   /* init 전: 시작 용량 */
    printf("시작 pool=%lu entries, corpus=%.1f MB\n\n", m0.bt_pool_n, len/(1024.0*1024.0));
    printf("  %-8s %9s %12s %5s %9s %8s %s\n",
           "train", "train_t", "entries", "full", "peak_MB", "ratio", "lossless");

    /* 학습량 sweep: argv 또는 기본 {4M, 10M} */
    size_t def[] = { 4000000, 10000000 };
    size_t ndef = 2;
    int all_ok = 1;
    for (size_t i = 0; i < ndef; i++) {
        size_t tl = (argc>1 && i<(size_t)(argc-1)) ? (size_t)strtoul(argv[i+1],NULL,10) : def[i];
        if (tl + EX_LEN > len) tl = len - EX_LEN;
        const uint8_t *ex = c + tl;

        CecBT bt = btv3_cec_bt();
        clock_t t = clock();
        for (size_t k = 0; k < tl; k++) bt.train(c[k], bt.user);
        double tt = (double)(clock()-t)/CLOCKS_PER_SEC;
        unsigned long ent = bt_v3_entries();
        BtV3Mem mp; bt_v3_footprint(&mp);       /* 학습 후 peak 용량 */
        int full = (ent >= mp.bt_pool_n);

        CecEncoder *e = cec_enc_new(&bt);
        for (size_t k = 0; k < EX_LEN; k++) cec_enc_byte(e, ex[k]);
        size_t cl = 0; uint8_t *cmp = cec_enc_finish(e, &cl); cec_enc_free(e);

        CecBT bd = btv3_cec_bt();
        for (size_t k = 0; k < tl; k++) bd.train(c[k], bd.user);
        uint8_t *dec = cec_decompress(cmp, cl, EX_LEN, &bd);
        int ok = (dec && memcmp(dec, ex, EX_LEN)==0);
        all_ok = all_ok && ok;
        free(dec); free(cmp);

        char lbl[24]; snprintf(lbl, sizeof(lbl), "%zuMB", tl/1000000);
        printf("  %-8s %8.2fs %12lu %5s %8.0f %7.2fx %s\n",
               lbl, tt, ent, full?"YES":"no", mp.total/(1024.0*1024.0),
               cl?(double)EX_LEN/cl:0, ok?"yes":"NO");
    }
    free(c);
    printf("\n%s\n", all_ok ? "all lossless" : "LOSSLESS BROKEN");
    return all_ok ? 0 : 1;
}
