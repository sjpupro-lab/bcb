/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* test_v4_aux.c — v4 byte_type distribution blend ablation
 *
 * 각 책에서 50KB 학습 후 4KB 발췌를 압축:
 *   baseline = v1a (BT + 합=1)
 *   +aux     = v1a + byte_type blend (α)
 * 압축 바이트와 개선율, round-trip 무손실을 보고한다.
 * 보고서(2026-05-26)의 +0.41% 일관 개선을 레포에서 재현하는 것이 목적.
 */
#include "ce_compress.h"
#include "symdist.h"
#include "aux.h"
#include "bt_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALPHA   0.985
#define EX_OFF  60000
#define EX_LEN  4096
#define TRAIN   50000

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t*)malloc((size_t)sz);
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    *out_len = n; return buf;
}

/* v1a (symdist only) */
static size_t run_base(const uint8_t *c, size_t toff, size_t tl,
                       const uint8_t *ex, size_t exlen, int *ok) {
    CecBT bt = symdist_bt();
    for (size_t i = 0; i < tl; i++) bt_v4_train(c[toff + i]);
    CecEncoder *e = cec_enc_new(&bt);
    for (size_t i = 0; i < exlen; i++) cec_enc_byte(e, ex[i]);
    size_t clen = 0; uint8_t *comp = cec_enc_finish(e, &clen); cec_enc_free(e);

    CecBT bt2 = symdist_bt();
    for (size_t i = 0; i < tl; i++) bt_v4_train(c[toff + i]);
    uint8_t *dec = cec_decompress(comp, clen, exlen, &bt2);
    *ok = (dec && memcmp(dec, ex, exlen) == 0);
    free(dec); free(comp);
    return clen;
}

/* v1a + byte_type blend */
static size_t run_aux(AuxChannel *ch, const uint8_t *c, size_t toff, size_t tl,
                      const uint8_t *ex, size_t exlen, int *ok) {
    CecBT bt = aux_cec_bt(ch);
    aux_prime(ch, c + toff, tl);
    CecEncoder *e = cec_enc_new(&bt);
    for (size_t i = 0; i < exlen; i++) cec_enc_byte(e, ex[i]);
    size_t clen = 0; uint8_t *comp = cec_enc_finish(e, &clen); cec_enc_free(e);

    CecBT bt2 = aux_cec_bt(ch);
    aux_prime(ch, c + toff, tl);
    uint8_t *dec = cec_decompress(comp, clen, exlen, &bt2);
    *ok = (dec && memcmp(dec, ex, exlen) == 0);
    free(dec); free(comp);
    return clen;
}

int main(int argc, char **argv) {
    const char *books[] = {
        "tests/corpus/pride_and_prejudice.txt",
        "tests/corpus/frankenstein.txt",
        "tests/corpus/alice_in_wonderland.txt",
        "tests/corpus/moby_dick.txt",
    };
    const char *names[] = { "pride", "frankenstein", "alice", "moby_dick" };
    int n = (argc > 1) ? 1 : 4;
    if (argc > 1) { books[0] = argv[1]; names[0] = argv[1]; }

    AuxChannel *ch = aux_byte_type_new(ALPHA);
    printf("v4 byte_type blend (alpha=%.3f), %dKB train / %dB excerpt @ off %d\n\n",
           ALPHA, TRAIN/1000, EX_LEN, EX_OFF);
    printf("  %-14s %8s %8s %9s %s\n", "book", "base(B)", "aux(B)", "개선", "lossless");

    size_t sum_b = 0, sum_a = 0; int all_ok = 1;
    for (int i = 0; i < n; i++) {
        size_t len = 0; uint8_t *c = slurp(books[i], &len);
        if (!c || len < EX_OFF + EX_LEN) { printf("  [SKIP] %s\n", names[i]); free(c); continue; }
        const uint8_t *ex = c + EX_OFF;
        int ok0 = 0, ok1 = 0;
        size_t cb = run_base(c, EX_OFF - TRAIN, TRAIN, ex, EX_LEN, &ok0);
        size_t ca = run_aux(ch, c, EX_OFF - TRAIN, TRAIN, ex, EX_LEN, &ok1);
        all_ok = all_ok && ok0 && ok1;
        sum_b += cb; sum_a += ca;
        double impr = cb ? 100.0 * (double)((long)cb - (long)ca) / (double)cb : 0.0;
        printf("  %-14s %8zu %8zu %+8.2f%% %s\n", names[i], cb, ca, impr,
               (ok0 && ok1) ? "yes" : "NO");
        free(c);
    }
    double tot = sum_b ? 100.0 * (double)((long)sum_b - (long)sum_a) / (double)sum_b : 0.0;
    printf("  %-14s %8zu %8zu %+8.2f%%\n", "합계", sum_b, sum_a, tot);
    aux_free(ch);
    printf("\n%s\n", all_ok ? "all lossless" : "LOSSLESS BROKEN");
    return all_ok ? 0 : 1;
}
