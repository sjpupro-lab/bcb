/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* test_v3_scale.c — v3 open addressing 대규모 학습 스케일링
 *
 * moby_dick 으로 학습량을 키우며 학습/인코드 시간·압축비·무손실을 측정.
 * v0(chained) 는 200K 까지만 측정(시간 폭발 회피), v3(open)는 1MB 까지.
 * 보고서의 "500KB 시간 폭발" 이 open addressing 으로 해소됨을 보인다.
 */
#include "ce_compress.h"
#include "bt_model.h"
#include "btv3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EX_LEN 4096
#define V0_MAX_TRAIN 200000   /* v0 는 여기까지만 측정 */

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t*)malloc((size_t)sz);
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    *out_len = n; return buf;
}

int main(void) {
    const char *path = "tests/corpus/moby_dick.txt";
    size_t len = 0; uint8_t *c = slurp(path, &len);
    if (!c) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    size_t sizes[] = { 50000, 200000, 500000, 1000000 };
    int nsz = sizeof(sizes)/sizeof(sizes[0]);

    printf("v3 대규모 학습 스케일링 (%s, %zu bytes), 발췌 %dB\n\n", path, len, EX_LEN);
    printf("  %-9s %10s %10s %10s %8s %10s %s\n",
           "train", "v0_train", "v3_train", "v3_enc", "v3_ratio", "v3_entries", "lossless");

    int all_ok = 1;
    for (int i = 0; i < nsz; i++) {
        size_t tl = sizes[i];
        if (tl + EX_LEN > len) { printf("  [SKIP] %zuKB (corpus too small)\n", tl/1000); continue; }
        const uint8_t *ex = c + tl;

        /* v0 학습 시간 (<= V0_MAX_TRAIN 만) */
        double v0t = -1;
        if (tl <= V0_MAX_TRAIN) {
            CecBT b0 = cec_default_bt();
            clock_t t = clock();
            for (size_t k = 0; k < tl; k++) b0.train(c[k], b0.user);
            v0t = (double)(clock()-t)/CLOCKS_PER_SEC;
        }

        /* v3 학습 시간 */
        CecBT b3 = btv3_cec_bt();
        clock_t t = clock();
        for (size_t k = 0; k < tl; k++) b3.train(c[k], b3.user);
        double v3t = (double)(clock()-t)/CLOCKS_PER_SEC;
        unsigned long ent = bt_v3_entries();

        /* v3 인코드 시간 + 압축비 */
        clock_t te = clock();
        CecEncoder *enc = cec_enc_new(&b3);
        for (size_t k = 0; k < EX_LEN; k++) cec_enc_byte(enc, ex[k]);
        size_t cl = 0; uint8_t *cmp = cec_enc_finish(enc, &cl); cec_enc_free(enc);
        double v3e = (double)(clock()-te)/CLOCKS_PER_SEC;

        /* v3 무손실 */
        CecBT bd = btv3_cec_bt();
        for (size_t k = 0; k < tl; k++) bd.train(c[k], bd.user);
        uint8_t *dec = cec_decompress(cmp, cl, EX_LEN, &bd);
        int ok = (dec && memcmp(dec, ex, EX_LEN) == 0);
        all_ok = all_ok && ok;
        free(dec); free(cmp);

        char v0s[24], lbl[24];
        if (v0t < 0) snprintf(v0s, sizeof(v0s), "%10s", "—(skip)");
        else         snprintf(v0s, sizeof(v0s), "%9.3fs", v0t);
        snprintf(lbl, sizeof(lbl), "%zuKB", tl/1000);
        printf("  %-9s %10s %9.3fs %9.3fs %7.2fx %10lu %s\n",
               lbl, v0s, v3t, v3e, cl?(double)EX_LEN/cl:0, ent, ok?"yes":"NO");
    }
    free(c);
    printf("\n%s\n", all_ok ? "all lossless" : "LOSSLESS BROKEN");
    return all_ok ? 0 : 1;
}
