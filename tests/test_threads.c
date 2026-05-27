/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* test_threads.c — 멀티스레드 동시 encode/decode round-trip 무손실 (de-globalize 검증).
 *   test_threads <prior> <data-file> [threads] [iters]
 * 스레드마다 자기 encoder/decoder 를 (읽기 전용으로 공유되는) 동일 prior 로 만들고,
 * 서로 다른 메시지 슬라이스를 반복 압축·복원하며 무손실을 확인한다. 전역 코덱 상태가
 * 남아 있으면 동시 실행에서 손상/불일치가 발생한다. */
#include "bcb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct {
    BcbPrior *p;
    const unsigned char *data; size_t data_len;
    size_t off, msglen; long iters; int id;
    int fail;
} Arg;

static void *worker(void *vp) {
    Arg *a = (Arg *)vp;
    BcbEncoder *e = bcb_encoder_new(a->p);
    BcbDecoder *d = bcb_decoder_new(a->p);
    if (!e || !d) { a->fail = 1; return NULL; }
    size_t cap = a->msglen + 64;
    uint8_t *out = malloc(cap), *back = malloc(a->msglen + 1);
    const unsigned char *msg = a->data + a->off;
    for (long i = 0; i < a->iters && !a->fail; i++) {
        ssize_t n = bcb_encode(e, msg, a->msglen, out, cap);
        if (n <= 0) { a->fail = 1; break; }
        ssize_t m = bcb_decode(d, out, (size_t)n, back, a->msglen);
        if (m != (ssize_t)a->msglen || memcmp(back, msg, a->msglen) != 0) { a->fail = 1; break; }
    }
    free(out); free(back);
    bcb_encoder_free(e); bcb_decoder_free(d);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <prior> <data> [threads] [iters]\n", argv[0]); return 2; }
    int nthreads = argc > 3 ? atoi(argv[3]) : 8;
    long iters = argc > 4 ? atol(argv[4]) : 4000;
    if (nthreads < 1) nthreads = 1; if (nthreads > 64) nthreads = 64;

    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)sz); fread(data, 1, (size_t)sz, f); fclose(f);

    BcbPrior *p = bcb_prior_open(argv[1]);   /* 단일 스레드에서 open (LUT 1회 생성) */
    if (!p) { fprintf(stderr, "cannot open prior %s\n", argv[1]); return 1; }

    size_t msglen = 256;
    if ((size_t)sz < msglen * (size_t)nthreads + msglen) { msglen = (size_t)sz / (nthreads + 1); }
    if (msglen < 16) { fprintf(stderr, "data too small\n"); return 1; }

    pthread_t th[64]; Arg args[64];
    for (int i = 0; i < nthreads; i++) {
        args[i].p = p; args[i].data = data; args[i].data_len = (size_t)sz;
        args[i].off = (size_t)i * msglen;           /* 스레드마다 다른 슬라이스 */
        args[i].msglen = msglen; args[i].iters = iters; args[i].id = i; args[i].fail = 0;
        pthread_create(&th[i], NULL, worker, &args[i]);
    }
    int fails = 0;
    for (int i = 0; i < nthreads; i++) { pthread_join(th[i], NULL); fails += args[i].fail; }

    bcb_prior_close(p); free(data);
    printf("threads=%d iters=%ld msglen=%zu : %s (%d thread failure%s)\n",
           nthreads, iters, msglen, fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
