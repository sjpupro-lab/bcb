/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* test_v3_compare.c — v0 vs v3 (distribution caching) 검증
 *
 * 4권 각각 50KB 학습 후 4KB 발췌를 압축:
 *   - 분포 동등성: 학습 직후 cum[] 의 v0 vs v3 최대 상대오차
 *   - 압축비: v3 가 v0 대비 ±0.5% 이내인지
 *   - 무손실: v3 round-trip
 *   - 속도: 인코드 CPU 시간 v0 vs v3 (speedup)
 */
#include "ce_compress.h"
#include "bt_model.h"
#include "btv3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EX_OFF  60000
#define EX_LEN  4096
#define TRAIN   50000
#define TOL_PCT 0.5

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
    const char *books[4] = {
        "tests/corpus/pride_and_prejudice.txt",
        "tests/corpus/frankenstein.txt",
        "tests/corpus/alice_in_wonderland.txt",
        "tests/corpus/moby_dick.txt",
    };
    const char *names[4] = { "pride", "frankenstein", "alice", "moby_dick" };

    printf("v0 vs v3 (distribution caching), %dKB train / %dB excerpt @ off %d\n\n",
           TRAIN/1000, EX_LEN, EX_OFF);
    printf("  %-13s %8s %8s %8s %9s %9s %8s %s\n",
           "book", "v0(B)", "v3(B)", "ratioΔ", "cum_relΔ", "v0_t", "v3_t", "speed/lossless");

    int all_ok = 1;
    double sum_v0t = 0, sum_v3t = 0; size_t tv0 = 0, tv3 = 0;
    for (int i = 0; i < 4; i++) {
        size_t len = 0; uint8_t *c = slurp(books[i], &len);
        if (!c || len < EX_OFF + EX_LEN) { printf("  [SKIP] %s\n", names[i]); free(c); continue; }
        const uint8_t *ex = c + EX_OFF;
        size_t toff = EX_OFF - TRAIN;

        /* ── v0 학습 ── */
        CecBT b0 = cec_default_bt();
        for (size_t k = 0; k < TRAIN; k++) b0.train(c[toff + k], b0.user);
        uint32_t cum0[257]; bt_v4_distribution(cum0, CEC_RC_SCALE);

        /* ── v3 학습 ── */
        CecBT b3 = btv3_cec_bt();
        for (size_t k = 0; k < TRAIN; k++) b3.train(c[toff + k], b3.user);
        uint32_t cum3[257]; bt_v3_distribution(cum3, CEC_RC_SCALE);

        /* 분포 동등성: 폭(width) 상대오차 최대 */
        double max_rel = 0;
        for (int b = 0; b < 256; b++) {
            double w0 = (double)(cum0[b+1]-cum0[b]);
            double w3 = (double)(cum3[b+1]-cum3[b]);
            double d = w0>0 ? (w0>w3?w0-w3:w3-w0)/w0 : 0;
            if (d > max_rel) max_rel = d;
        }

        /* ── v0 인코드 (시간 측정, 학습 재시작) ── */
        CecBT e0 = cec_default_bt();
        for (size_t k = 0; k < TRAIN; k++) e0.train(c[toff + k], e0.user);
        clock_t t0 = clock();
        CecEncoder *enc0 = cec_enc_new(&e0);
        for (size_t k = 0; k < EX_LEN; k++) cec_enc_byte(enc0, ex[k]);
        size_t cl0 = 0; uint8_t *cmp0 = cec_enc_finish(enc0, &cl0); cec_enc_free(enc0);
        double v0t = (double)(clock() - t0) / CLOCKS_PER_SEC;
        free(cmp0);

        /* ── v3 인코드 (시간 측정) ── */
        CecBT e3 = btv3_cec_bt();
        for (size_t k = 0; k < TRAIN; k++) e3.train(c[toff + k], e3.user);
        clock_t t3 = clock();
        CecEncoder *enc3 = cec_enc_new(&e3);
        for (size_t k = 0; k < EX_LEN; k++) cec_enc_byte(enc3, ex[k]);
        size_t cl3 = 0; uint8_t *cmp3 = cec_enc_finish(enc3, &cl3); cec_enc_free(enc3);
        double v3t = (double)(clock() - t3) / CLOCKS_PER_SEC;

        /* ── v3 무손실 ── */
        CecBT d3 = btv3_cec_bt();
        for (size_t k = 0; k < TRAIN; k++) d3.train(c[toff + k], d3.user);
        uint8_t *dec = cec_decompress(cmp3, cl3, EX_LEN, &d3);
        int ok = (dec && memcmp(dec, ex, EX_LEN) == 0);
        free(dec); free(cmp3);

        double ratio_delta = cl0 ? 100.0 * ((double)cl3 - (double)cl0) / (double)cl0 : 0;
        int within = (ratio_delta <= TOL_PCT && ratio_delta >= -TOL_PCT);
        double speed = v3t > 0 ? v0t / v3t : 0;
        all_ok = all_ok && ok && within;
        sum_v0t += v0t; sum_v3t += v3t; tv0 += cl0; tv3 += cl3;

        printf("  %-13s %8zu %8zu %+7.2f%% %8.4f%% %8.3fs %7.3fs  %5.1fx %s%s\n",
               names[i], cl0, cl3, ratio_delta, max_rel*100.0, v0t, v3t, speed,
               ok ? "L" : "LOSSY", within ? "" : " RATIO!");
        free(c);
    }
    double tot_delta = tv0 ? 100.0*((double)tv3-(double)tv0)/(double)tv0 : 0;
    double tot_speed = sum_v3t>0 ? sum_v0t/sum_v3t : 0;
    printf("  %-13s %8zu %8zu %+7.2f%% %8s %8.3fs %7.3fs  %5.1fx\n",
           "합계", tv0, tv3, tot_delta, "-", sum_v0t, sum_v3t, tot_speed);

    printf("\n%s (tol ±%.1f%%)\n", all_ok ? "PASS — lossless & within tolerance" : "FAIL", TOL_PCT);
    return all_ok ? 0 : 1;
}
