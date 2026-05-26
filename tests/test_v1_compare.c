/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* test_v1_compare.c — v0 vs v1(a) 합=1 강제 ablation
 *
 * 동일 코퍼스 prefix 로 학습 후 동일 발췌를 압축, 두 변형의 압축비/무손실 비교.
 * v0 와 v1 은 bt_v4 전역 상태를 공유하므로 순차 실행한다.
 */
#include "ce_compress.h"
#include "symdist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t*)malloc((size_t)sz);
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    *out_len = n; return buf;
}

/* BT 를 prefix 로 학습 후 excerpt 인코드, 압축 길이 반환 + round-trip 검증.
 *
 * v0/v1 BT 는 bt_v4 전역 상태를 공유하고 make() 가 bt_v4_init() 으로 리셋한다.
 * 따라서 디코더용 BT 는 반드시 인코딩이 끝난 "뒤" 에 make() 로 다시 만들어야
 * 동일한 초기 상태에서 출발한다 (인코딩 중 train 으로 상태가 변하므로). */
static size_t run(CecBT (*make)(void),
                  const uint8_t *corpus, size_t train_off, size_t train_len,
                  const uint8_t *ex, size_t ex_len, int *ok) {
    CecBT bt = make();
    for (size_t i = 0; i < train_len; i++) bt.train(corpus[train_off + i], bt.user);
    CecEncoder *e = cec_enc_new(&bt);
    for (size_t i = 0; i < ex_len; i++) cec_enc_byte(e, ex[i]);
    size_t clen = 0;
    uint8_t *comp = cec_enc_finish(e, &clen);
    cec_enc_free(e);

    CecBT bt2 = make();   /* 인코딩 후 재초기화 — 디코더는 같은 시작점에서 학습 */
    for (size_t i = 0; i < train_len; i++) bt2.train(corpus[train_off + i], bt2.user);
    uint8_t *dec = cec_decompress(comp, clen, ex_len, &bt2);
    *ok = (dec && memcmp(dec, ex, ex_len) == 0);
    free(dec); free(comp);
    return clen;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/corpus/pride_and_prejudice.txt";
    size_t clen_corpus = 0;
    uint8_t *corpus = slurp(path, &clen_corpus);
    if (!corpus) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    size_t ex_off = 600000, ex_len = 4096;
    if (ex_off + ex_len > clen_corpus) { ex_len = clen_corpus < ex_len ? clen_corpus : ex_len; ex_off = clen_corpus - ex_len; }
    const uint8_t *ex = corpus + ex_off;

    printf("corpus: %s, excerpt off=%zu len=%zu\n\n", path, ex_off, ex_len);
    printf("  %-8s %10s %8s %10s %8s\n", "train", "v0(B)", "v0", "v1a(B)", "v1a");
    size_t trains[] = { 0, 50000, 200000, 500000 };
    int all_ok = 1;
    for (size_t t = 0; t < sizeof(trains)/sizeof(trains[0]); t++) {
        size_t tl = trains[t];
        if (tl > ex_off) continue;
        size_t toff = ex_off - tl;

        int ok0 = 0, ok1 = 0;
        size_t c0 = run(cec_default_bt, corpus, toff, tl, ex, ex_len, &ok0);
        size_t c1 = run(symdist_bt,     corpus, toff, tl, ex, ex_len, &ok1);
        all_ok = all_ok && ok0 && ok1;

        char lbl[32]; snprintf(lbl, sizeof(lbl), "%zuKB", tl/1000);
        printf("  %-8s %10zu %7.3fx %10zu %7.3fx  %s\n",
               lbl, c0, (double)ex_len/c0, c1, (double)ex_len/c1,
               (ok0 && ok1) ? "lossless" : "LOSSY!");
    }
    free(corpus);
    printf("\n%s\n", all_ok ? "all lossless" : "LOSSLESS BROKEN");
    return all_ok ? 0 : 1;
}
