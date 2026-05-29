/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* bcb-cli.c — 표준 압축 CLI
 *
 *   bcb encode <in> <out> [-t train.txt]
 *   bcb decode <in> <out> [-t train.txt]
 *
 * -t 로 준 학습 파일은 encode/decode 양쪽에서 동일해야 한다 (공유 prior).
 * 컨테이너 포맷: "BCB1" magic(4) + orig_len(8, LE) + range-coded payload.
 */
#include "ce_compress.h"
#include "btv3.h"
#include "bcb_prior.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t*)malloc((size_t)sz ? (size_t)sz : 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_len = n;
    return buf;
}

static int dump(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(data, 1, len, f);
    fclose(f);
    return 0;
}

static void prime(CecBT *bt, const char *train_path) {
    if (!train_path) return;
    size_t tl = 0;
    uint8_t *t = slurp(train_path, &tl);
    if (!t) { fprintf(stderr, "warning: cannot read training file %s\n", train_path); return; }
    for (size_t i = 0; i < tl; i++) bt->train(t[i], bt->user);
    free(t);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage:\n"
                "  %s encode <in> <out> [-t train.txt | --prior file.bcb-prior]\n"
                "  %s decode <in> <out> [-t train.txt | --prior file.bcb-prior]\n",
                argv[0], argv[0]);
        return 2;
    }
    const char *mode = argv[1], *in = argv[2], *out = argv[3], *train = NULL, *prior_path = NULL;
    for (int i = 4; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-t") == 0) train = argv[i+1];
        else if (strcmp(argv[i], "--prior") == 0) prior_path = argv[i+1];
    }

    /* --prior: mmap 된 frozen v3 prior 사용 (encode/decode 양쪽 동일 파일). */
    BcbPrior *prior = NULL;
    if (prior_path) {
        prior = bcb_prior_mmap(prior_path);
        if (!prior) { fprintf(stderr, "cannot mmap prior %s\n", prior_path); return 1; }
    }

    size_t in_len = 0;
    uint8_t *in_buf = slurp(in, &in_len);
    if (!in_buf) { fprintf(stderr, "cannot read %s\n", in); return 1; }

    if (strcmp(mode, "encode") == 0) {
        CecBT bt;
        if (prior) bt = bcb_prior_cec_bt(prior);    /* attach 내부 수행 */
        else { bt = cec_default_bt(); prime(&bt, train); }
        CecEncoder *e = cec_enc_new(&bt);
        for (size_t i = 0; i < in_len; i++) cec_enc_byte(e, in_buf[i]);
        size_t clen = 0;
        uint8_t *comp = cec_enc_finish(e, &clen);
        cec_enc_free(e);

        size_t total = 12 + clen;
        uint8_t *outbuf = (uint8_t*)malloc(total);
        memcpy(outbuf, "BCB1", 4);
        uint64_t ol = (uint64_t)in_len;
        for (int i = 0; i < 8; i++) outbuf[4 + i] = (uint8_t)(ol >> (8*i));
        memcpy(outbuf + 12, comp, clen);
        if (dump(out, outbuf, total) != 0) { fprintf(stderr, "cannot write %s\n", out); return 1; }
        fprintf(stderr, "encoded %zu -> %zu bytes (%.2fx)\n", in_len, total, clen ? (double)in_len/(double)total : 0.0);
        free(comp); free(outbuf);
    } else if (strcmp(mode, "decode") == 0) {
        if (in_len < 12 || memcmp(in_buf, "BCB1", 4) != 0) { fprintf(stderr, "not a BCB1 file\n"); return 1; }
        uint64_t ol = 0;
        for (int i = 0; i < 8; i++) ol |= (uint64_t)in_buf[4 + i] << (8*i);
        CecBT bt;
        if (prior) bt = bcb_prior_cec_bt(prior);
        else { bt = cec_default_bt(); prime(&bt, train); }
        uint8_t *dec = cec_decompress(in_buf + 12, in_len - 12, (size_t)ol, &bt);
        if (!dec) { fprintf(stderr, "decode failed\n"); return 1; }
        if (dump(out, dec, (size_t)ol) != 0) { fprintf(stderr, "cannot write %s\n", out); return 1; }
        fprintf(stderr, "decoded -> %llu bytes\n", (unsigned long long)ol);
        free(dec);
    } else {
        fprintf(stderr, "unknown mode '%s'\n", mode);
        return 2;
    }
    if (prior) bcb_prior_close(prior);
    free(in_buf);
    return 0;
}
