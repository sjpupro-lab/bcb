/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* test_v4_aux.c — v4 보조채널 ablation
 *
 * baseline = v1a (BT + 합=1). 각 채널 단독 + 4채널 combo 의 개선율을 4권에서 측정.
 *   byte_type / bigram_type / case_pattern / whitespace_phase / combo(4)
 * 모든 변형은 round-trip 무손실을 검증한다.
 */
#include "ce_compress.h"
#include "aux_channel.h"
#include "btv3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALPHA   0.985
#define EX_OFF  60000
#define EX_LEN  4096
#define TRAIN   50000
#define NB      4
#define NV      5

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t*)malloc((size_t)sz);
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    *out_len = n; return buf;
}

/* ch==NULL → baseline(v1a). 아니면 aux_cec_bt + aux_prime. round-trip 검증. */
static size_t run(AuxChannel *ch, const uint8_t *c, size_t toff, size_t tl,
                  const uint8_t *ex, size_t exlen, int *ok) {
    CecBT bt;
    if (ch) { bt = aux_cec_bt(ch); aux_prime(ch, c + toff, tl); }
    else    { bt = btv3_cec_bt(); for (size_t i = 0; i < tl; i++) bt_v3_train(c[toff + i]); }
    CecEncoder *e = cec_enc_new(&bt);
    for (size_t i = 0; i < exlen; i++) cec_enc_byte(e, ex[i]);
    size_t clen = 0; uint8_t *comp = cec_enc_finish(e, &clen); cec_enc_free(e);

    CecBT bt2;
    if (ch) { bt2 = aux_cec_bt(ch); aux_prime(ch, c + toff, tl); }
    else    { bt2 = btv3_cec_bt(); for (size_t i = 0; i < tl; i++) bt_v3_train(c[toff + i]); }
    uint8_t *dec = cec_decompress(comp, clen, exlen, &bt2);
    *ok = (dec && memcmp(dec, ex, exlen) == 0);
    free(dec); free(comp);
    return clen;
}

int main(void) {
    const char *books[NB] = {
        "tests/corpus/pride_and_prejudice.txt",
        "tests/corpus/frankenstein.txt",
        "tests/corpus/alice_in_wonderland.txt",
        "tests/corpus/moby_dick.txt",
    };
    const char *bnames[NB] = { "pride", "frankenstein", "alice", "moby_dick" };
    const char *vnames[NV] = { "byte_type", "bigram", "case", "whitespace", "combo(4)" };

    AuxChannel *byte = aux_byte_type_new(ALPHA);
    AuxChannel *bigr = aux_bigram_type_new(ALPHA);
    AuxChannel *cse  = aux_case_pattern_new(ALPHA);
    AuxChannel *ws   = aux_whitespace_phase_new(ALPHA);
    AuxChannel *all[4] = { byte, bigr, cse, ws };
    AuxChannel *combo = aux_combo_new(all, 4);
    AuxChannel *variants[NV] = { byte, bigr, cse, ws, combo };

    size_t base[NB]; size_t auxb[NV][NB];
    int all_ok = 1;

    printf("v4 보조채널 ablation (alpha=%.3f), %dKB train / %dB excerpt @ off %d\n\n",
           ALPHA, TRAIN/1000, EX_LEN, EX_OFF);

    for (int bk = 0; bk < NB; bk++) {
        size_t len = 0; uint8_t *c = slurp(books[bk], &len);
        if (!c || len < EX_OFF + EX_LEN) { fprintf(stderr, "skip %s\n", bnames[bk]); base[bk] = 0; free(c); continue; }
        const uint8_t *ex = c + EX_OFF;
        int ok = 0;
        base[bk] = run(NULL, c, EX_OFF - TRAIN, TRAIN, ex, EX_LEN, &ok);
        all_ok = all_ok && ok;
        for (int v = 0; v < NV; v++) {
            auxb[v][bk] = run(variants[v], c, EX_OFF - TRAIN, TRAIN, ex, EX_LEN, &ok);
            all_ok = all_ok && ok;
        }
        free(c);
    }

    /* 개선율 매트릭스 (%, 양수=더 작아짐) */
    printf("  %-13s %8s", "book", "base(B)");
    for (int v = 0; v < NV; v++) printf(" %10s", vnames[v]);
    printf("\n");
    size_t tb = 0, ta[NV] = {0};
    for (int bk = 0; bk < NB; bk++) {
        if (!base[bk]) continue;
        printf("  %-13s %8zu", bnames[bk], base[bk]);
        tb += base[bk];
        for (int v = 0; v < NV; v++) {
            double impr = 100.0 * (double)((long)base[bk] - (long)auxb[v][bk]) / (double)base[bk];
            printf(" %+9.2f%%", impr);
            ta[v] += auxb[v][bk];
        }
        printf("\n");
    }
    printf("  %-13s %8zu", "합계", tb);
    for (int v = 0; v < NV; v++) {
        double impr = tb ? 100.0 * (double)((long)tb - (long)ta[v]) / (double)tb : 0.0;
        printf(" %+9.2f%%", impr);
    }
    printf("\n\n%s\n", all_ok ? "all lossless" : "LOSSLESS BROKEN");

    aux_free(combo); aux_free(byte); aux_free(bigr); aux_free(cse); aux_free(ws);
    return all_ok ? 0 : 1;
}
