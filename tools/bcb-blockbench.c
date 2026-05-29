/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* bcb-blockbench.c — frozen mmap prior 로 length-delimited 블록들을 압축.
 *
 *   bcb-blockbench --prior <file> --blocks <file>
 *
 * blocks 파일 포맷: [u32 LE len][len bytes] 반복 (EOF 까지).
 * 각 블록을 빈 window 에서 독립 인코딩(stateless), round-trip 무손실 검증.
 * stdout: 블록마다 "orig comp" 한 줄. stderr: 요약(lossless, 총합, 처리량).
 *
 * HPACK 비교용(tools/bcb_vs_hpack.py)에서 BCB per-block 바이트를 얻는다.
 */
#define _POSIX_C_SOURCE 200809L
#include "ce_compress.h"
#include "btv3.h"
#include "bcb_prior.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int read_u32(FILE *f, uint32_t *out) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}

int main(int argc, char **argv) {
    const char *prior_path = NULL, *blocks_path = NULL;
    long random_n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prior") && i+1<argc) prior_path = argv[++i];
        else if (!strcmp(argv[i], "--blocks") && i+1<argc) blocks_path = argv[++i];
        else if (!strcmp(argv[i], "--random") && i+1<argc) random_n = strtol(argv[++i], NULL, 10);
    }
    if (!prior_path || !blocks_path) {
        fprintf(stderr, "usage: %s --prior <file> --blocks <file> [--random N]\n", argv[0]);
        return 2;
    }

    BcbPrior *p = bcb_prior_mmap(prior_path);
    if (!p) { fprintf(stderr, "cannot mmap prior %s\n", prior_path); return 1; }
    FILE *bf = fopen(blocks_path, "rb");
    if (!bf) { fprintf(stderr, "cannot read %s\n", blocks_path); bcb_prior_close(p); return 1; }

    /* 모든 블록의 압축본/원본을 보관 (message-level random access 시연용) */
    size_t cap = 1024, n = 0;
    unsigned char **comps = (unsigned char**)malloc(sizeof(*comps)*cap);
    size_t *clens = (size_t*)malloc(sizeof(*clens)*cap);
    unsigned char **origs = (unsigned char**)malloc(sizeof(*origs)*cap);
    uint32_t *olens = (uint32_t*)malloc(sizeof(*olens)*cap);

    size_t total_orig = 0, total_comp = 0;
    int lossless = 1;
    double enc_sec = 0;
    uint32_t len;
    while (read_u32(bf, &len)) {
        unsigned char *msg = (unsigned char *)malloc(len ? len : 1);
        if (len && fread(msg, 1, len, bf) != len) { free(msg); break; }

        CecBT bt = bcb_prior_cec_bt(p);
        bt_v3_reset_window();
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        CecEncoder *e = cec_enc_new(&bt);
        for (uint32_t i = 0; i < len; i++) cec_enc_byte(e, msg[i]);
        size_t clen = 0;
        unsigned char *comp = cec_enc_finish(e, &clen);
        cec_enc_free(e);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        enc_sec += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

        CecBT btd = bcb_prior_cec_bt(p); bt_v3_reset_window();   /* fresh state (structural reset) */
        unsigned char *dec = cec_decompress(comp, clen, len, &btd);
        if (!dec || (len && memcmp(dec, msg, len) != 0)) lossless = 0;
        free(dec);

        printf("%u %zu\n", len, clen);
        if (n == cap) { cap*=2; comps=realloc(comps,sizeof(*comps)*cap); clens=realloc(clens,sizeof(*clens)*cap);
                        origs=realloc(origs,sizeof(*origs)*cap); olens=realloc(olens,sizeof(*olens)*cap); }
        comps[n]=comp; clens[n]=clen; origs[n]=msg; olens[n]=len; n++;
        total_orig += len; total_comp += clen;
    }
    fclose(bf);

    fprintf(stderr, "blocks=%zu lossless=%s total_orig=%zu total_comp=%zu ratio=%.3f enc_ms_per_block=%.3f blocks_per_s=%.0f\n",
            n, lossless ? "yes" : "NO", total_orig, total_comp,
            total_comp ? (double)total_orig / total_comp : 0.0,
            n ? enc_sec * 1000.0 / n : 0.0,
            enc_sec > 0 ? n / enc_sec : 0.0);

    /* message-level random access: 임의 블록을 다른 블록 디코드 없이 독립 복원.
     * (sub-message random access 는 sequential arithmetic coder 라 불가 — message 단위만.) */
    if (random_n > 0 && n > 0) {
        unsigned long seed = 0x9e3779b97f4a7c15UL; int ok = 1; double dsec = 0;
        long done = 0;
        for (long t = 0; t < random_n; t++) {
            seed = seed*6364136223846793005UL + 1442695040888963407UL;
            size_t idx = (size_t)((seed >> 33) % n);
            CecBT bt = bcb_prior_cec_bt(p); bt_v3_reset_window();
            struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
            unsigned char *dec = cec_decompress(comps[idx], clens[idx], olens[idx], &bt);
            clock_gettime(CLOCK_MONOTONIC,&t1);
            dsec += (t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
            if (!dec || (olens[idx] && memcmp(dec, origs[idx], olens[idx])!=0)) ok=0;
            free(dec); done++;
        }
        fprintf(stderr, "random-access: decoded %ld random blocks standalone, correct=%s, avg=%.1f us/block (position-independent)\n",
                done, ok?"yes":"NO", done? dsec*1e6/done : 0.0);
        if (!ok) lossless = 0;
    }

    for (size_t i=0;i<n;i++){ free(comps[i]); free(origs[i]); }
    free(comps); free(clens); free(origs); free(olens);
    bcb_prior_close(p);
    return lossless ? 0 : 3;
}
