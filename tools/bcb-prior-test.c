/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* bcb-prior-test.c — mmap prior 검증/측정.
 *
 *   bcb-prior-test equiv    <corpus> [--train-size N] [--msg-size S] [--msgs M]
 *       in-memory(frozen) vs mmap prior 가 비트 동일 출력인지 + round-trip 무손실 검증.
 *   bcb-prior-test rss-mem  <corpus> [--train-size N] [--msg-size S] [--msgs M]
 *       in-memory 학습 후 메시지 인코딩. 피크 RSS(VmHWM) + 처리량(msgs/s) 보고.
 *   bcb-prior-test rss-mmap <corpus> <prior> [--train-size N] [--msg-size S] [--msgs M]
 *       mmap prior 로 메시지 인코딩. 피크 RSS(VmHWM) + 처리량(msgs/s) 보고.
 *
 * 각 메시지는 빈 window 에서 독립 인코딩(stateless). rss-* 는 별도 프로세스로
 * 호출해 모드별 깨끗한 피크 RSS 를 얻는다.
 */
#define _POSIX_C_SOURCE 200809L
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
    if (sz <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    *out_len = n; return buf;
}

static long read_vmhwm_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256]; long kb = -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "VmHWM:", 6) == 0) { sscanf(line + 6, "%ld", &kb); break; }
    fclose(f); return kb;
}

/* frozen BT 로 메시지 1개 인코딩 → 압축 바이트수. comp_out!=NULL 이면 버퍼 반환(free 책임). */
static size_t enc_msg(CecBT *bt, const unsigned char *msg, size_t len, unsigned char **comp_out) {
    bt_v3_reset_window();
    CecEncoder *e = cec_enc_new(bt);
    for (size_t i = 0; i < len; i++) cec_enc_byte(e, msg[i]);
    size_t clen = 0;
    unsigned char *c = cec_enc_finish(e, &clen);
    cec_enc_free(e);
    if (comp_out) *comp_out = c; else free(c);
    return clen;
}

static int arg_get(int argc, char **argv, const char *key, long def, long *out) {
    *out = def;
    for (int i = 0; i + 1 < argc; i++) if (!strcmp(argv[i], key)) { *out = strtol(argv[i+1], NULL, 10); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s {equiv|rss-mem|rss-mmap} <corpus> [<prior>] "
                "[--train-size N] [--msg-size S] [--msgs M]\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    const char *corpus_path = argv[2];
    long train_size, msg_size, msgs;
    arg_get(argc, argv, "--train-size", 50000, &train_size);
    arg_get(argc, argv, "--msg-size", 128, &msg_size);
    arg_get(argc, argv, "--msgs", 200, &msgs);

    size_t corpus_len = 0;
    unsigned char *corpus = slurp(corpus_path, &corpus_len);
    if (!corpus) { fprintf(stderr, "cannot read %s\n", corpus_path); return 1; }
    size_t tl = (size_t)train_size < corpus_len ? (size_t)train_size : corpus_len/2;
    const unsigned char *test = corpus + tl;
    size_t test_len = corpus_len - tl;
    long max_msgs = (long)(test_len / (size_t)msg_size);
    if (msgs > max_msgs) msgs = max_msgs;
    if (msgs < 1) { fprintf(stderr, "corpus too small for msg-size %ld\n", msg_size); return 1; }

    if (!strcmp(mode, "equiv")) {
        const char *prior_path = "/tmp/bcb_equiv.bcb-prior";
        /* phase 1: in-memory 학습 + prior 저장 + frozen 인코드 (출력 저장).
         * btv3 는 단일 global 이므로 mmap attach 전에 in-memory 결과를 모두 모은다. */
        CecBT bt = btv3_cec_bt();
        for (size_t i = 0; i < tl; i++) bt.train(corpus[i], bt.user);
        if (bcb_prior_save(prior_path) != 0) { fprintf(stderr, "save failed\n"); return 1; }
        bt_v3_freeze(1);
        unsigned char **comp_mem = (unsigned char **)calloc(msgs, sizeof(*comp_mem));
        size_t *len_mem = (size_t *)calloc(msgs, sizeof(*len_mem));
        for (long k = 0; k < msgs; k++) {
            const unsigned char *m = test + (size_t)k * msg_size;
            len_mem[k] = enc_msg(&bt, m, msg_size, &comp_mem[k]);
        }
        bt_v3_free();                       /* in-memory pool 해제 → attach 안전 */

        /* phase 2: mmap prior 로 동일 메시지 인코드/복원, 비트 비교 */
        BcbPrior *p = bcb_prior_mmap(prior_path);
        if (!p) { fprintf(stderr, "mmap failed\n"); return 1; }
        int identical = 1, lossless = 1; long checked = 0;
        for (long k = 0; k < msgs; k++) {
            const unsigned char *m = test + (size_t)k * msg_size;
            CecBT btp = bcb_prior_cec_bt(p);
            unsigned char *cp = NULL; size_t lp = enc_msg(&btp, m, msg_size, &cp);
            if (lp != len_mem[k] || memcmp(cp, comp_mem[k], lp) != 0) identical = 0;
            bcb_prior_attach(p); bt_v3_reset_window();
            unsigned char *dec = cec_decompress(cp, lp, msg_size, &btp);
            if (!dec || memcmp(dec, m, msg_size) != 0) lossless = 0;
            free(dec); free(cp);
            checked++;
            if (!identical || !lossless) break;
        }
        printf("equiv  %-16s msg=%ld checked=%ld  bit-identical=%s  lossless=%s\n",
               corpus_path, msg_size, checked, identical?"yes":"NO", lossless?"yes":"NO");
        for (long k = 0; k < msgs; k++) free(comp_mem[k]);
        free(comp_mem); free(len_mem);
        bcb_prior_close(p);
        free(corpus);
        return (identical && lossless) ? 0 : 1;
    }

    if (!strcmp(mode, "rss-mem")) {
        CecBT bt = btv3_cec_bt();
        for (size_t i = 0; i < tl; i++) bt.train(corpus[i], bt.user);
        bt_v3_freeze(1);
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        size_t comp_total = 0, orig_total = 0;
        for (long k = 0; k < msgs; k++) {
            const unsigned char *m = test + (size_t)k * msg_size;
            comp_total += enc_msg(&bt, m, msg_size, NULL);
            orig_total += msg_size;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = (t1.tv_sec-t0.tv_sec) + (t1.tv_nsec-t0.tv_nsec)/1e9;
        printf("rss-mem   entries=%lu  VmHWM=%.2f MB  ratio=%.2fx  msgs/s=%.0f\n",
               bt_v3_entries(), read_vmhwm_kb()/1024.0,
               (double)orig_total/comp_total, msgs/sec);
        bt_v3_free(); free(corpus);
        return 0;
    }

    if (!strcmp(mode, "rss-mmap")) {
        if (argc < 4) { fprintf(stderr, "rss-mmap needs <prior>\n"); return 2; }
        const char *prior_path = argv[3];
        BcbPrior *p = bcb_prior_mmap(prior_path);
        if (!p) { fprintf(stderr, "mmap failed: %s\n", prior_path); return 1; }
        CecBT bt = bcb_prior_cec_bt(p);
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        size_t comp_total = 0, orig_total = 0;
        for (long k = 0; k < msgs; k++) {
            const unsigned char *m = test + (size_t)k * msg_size;
            comp_total += enc_msg(&bt, m, msg_size, NULL);
            orig_total += msg_size;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = (t1.tv_sec-t0.tv_sec) + (t1.tv_nsec-t0.tv_nsec)/1e9;
        printf("rss-mmap  VmHWM=%.2f MB  ratio=%.2fx  msgs/s=%.0f\n",
               read_vmhwm_kb()/1024.0, (double)orig_total/comp_total, msgs/sec);
        bcb_prior_close(p); free(corpus);
        return 0;
    }

    fprintf(stderr, "unknown mode '%s'\n", mode);
    free(corpus);
    return 2;
}
