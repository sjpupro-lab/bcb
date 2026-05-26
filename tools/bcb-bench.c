/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* bcb-bench.c — 자동 벤치마크
 *
 * 사용법: bcb-bench <corpus.txt> [excerpt_offset] [excerpt_len]
 *
 * 코퍼스의 한 발췌(기본 4KB)를 대상으로:
 *   - BCB 를 학습량 sweep (0 / 50K / 200K / 500K) 하며 압축비 측정
 *   - gzip / bzip2 / xz 와 비교 (시스템에 설치된 것만)
 *
 * 학습 데이터는 발췌 구간과 겹치지 않는 코퍼스 앞부분을 사용한다.
 * 인코더/디코더가 동일 코퍼스를 공유한다는 BCB 의 전제를 그대로 측정한다.
 */
#define _POSIX_C_SOURCE 200809L   /* popen/pclose (POSIX, not C99) */
#include "ce_compress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t*)malloc((size_t)sz);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_len = n;
    return buf;
}

/* corpus[train_off .. train_off+train_len) 로 BT 학습 후 excerpt 압축, 압축 길이 반환 */
static size_t bcb_compress_len(const uint8_t *corpus, size_t train_off, size_t train_len,
                               const uint8_t *excerpt, size_t ex_len, int *roundtrip_ok) {
    CecBT bt = cec_default_bt();
    for (size_t i = 0; i < train_len; i++) bt.train(corpus[train_off + i], bt.user);

    /* 학습된 상태에서 인코딩하려면 인코더를 직접 돌려야 한다 (cec_compress 는 fresh BT 가정 아님) */
    CecEncoder *e = cec_enc_new(&bt);
    for (size_t i = 0; i < ex_len; i++) cec_enc_byte(e, excerpt[i]);
    size_t clen = 0;
    uint8_t *comp = cec_enc_finish(e, &clen);
    cec_enc_free(e);

    /* 동일 학습 후 복원 검증 */
    CecBT bt2 = cec_default_bt();
    for (size_t i = 0; i < train_len; i++) bt2.train(corpus[train_off + i], bt2.user);
    uint8_t *dec = cec_decompress(comp, clen, ex_len, &bt2);
    *roundtrip_ok = (dec && memcmp(dec, excerpt, ex_len) == 0);
    free(dec);
    free(comp);
    return clen;
}

/* 시스템 압축기로 excerpt 를 압축한 바이트 수 (없으면 0) */
static size_t system_compress_len(const char *tool, const char *args,
                                  const uint8_t *excerpt, size_t ex_len) {
    const char *tmp = "/tmp/bcb_bench_excerpt.bin";
    FILE *tf = fopen(tmp, "wb");
    if (!tf) return 0;
    fwrite(excerpt, 1, ex_len, tf);
    fclose(tf);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "command -v %s >/dev/null 2>&1 && %s %s -c < %s 2>/dev/null | wc -c",
             tool, tool, args, tmp);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    long n = 0;
    if (fscanf(p, "%ld", &n) != 1) n = 0;
    pclose(p);
    return (size_t)n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus.txt> [excerpt_offset] [excerpt_len]\n", argv[0]);
        return 2;
    }
    size_t corpus_len = 0;
    uint8_t *corpus = slurp(argv[1], &corpus_len);
    if (!corpus) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

    size_t ex_off = (argc > 2) ? (size_t)strtoul(argv[2], NULL, 10) : 600000;
    size_t ex_len = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 10) : 4096;
    if (ex_off + ex_len > corpus_len) {
        /* 코퍼스가 작으면 발췌를 끝부분에 둔다 */
        if (corpus_len < ex_len) ex_len = corpus_len;
        ex_off = corpus_len - ex_len;
    }
    const uint8_t *excerpt = corpus + ex_off;

    printf("corpus: %s (%zu bytes)\n", argv[1], corpus_len);
    printf("excerpt: offset=%zu len=%zu\n\n", ex_off, ex_len);

    printf("BCB (codebook-free, BT prior):\n");
    printf("  %-12s %10s %8s %s\n", "train", "comp(B)", "ratio", "lossless");
    size_t train_sizes[] = { 0, 50000, 200000, 500000 };
    for (size_t t = 0; t < sizeof(train_sizes)/sizeof(train_sizes[0]); t++) {
        size_t tl = train_sizes[t];
        if (tl > ex_off) continue;   /* 발췌 앞부분만 학습에 사용 */
        size_t train_off = ex_off - tl;
        int ok = 0;
        size_t clen = bcb_compress_len(corpus, train_off, tl, excerpt, ex_len, &ok);
        double ratio = clen ? (double)ex_len / (double)clen : 0.0;
        char label[32];
        snprintf(label, sizeof(label), "%zuKB", tl/1000);
        printf("  %-12s %10zu %7.2fx %s\n", label, clen, ratio, ok ? "yes" : "NO");
    }

    printf("\nStandard compressors (excerpt only, no shared model):\n");
    printf("  %-12s %10s %8s\n", "tool", "comp(B)", "ratio");
    struct { const char *tool, *args, *label; } tools[] = {
        { "gzip",  "-9", "gzip-9" },
        { "bzip2", "-9", "bzip2-9" },
        { "xz",    "-9", "xz-9" },
        { "zstd",  "-19","zstd-19" },
    };
    for (size_t i = 0; i < sizeof(tools)/sizeof(tools[0]); i++) {
        size_t clen = system_compress_len(tools[i].tool, tools[i].args, excerpt, ex_len);
        if (clen == 0) { printf("  %-12s %10s\n", tools[i].label, "(n/a)"); continue; }
        double ratio = (double)ex_len / (double)clen;
        printf("  %-12s %10zu %7.2fx\n", tools[i].label, clen, ratio);
    }

    free(corpus);
    return 0;
}
