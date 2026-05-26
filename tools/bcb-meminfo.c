/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* bcb-meminfo.c — v3 BT 메모리 footprint + 작은 round-trip 점검
 *
 * 컴파일된 설정(데스크톱 / -DBCB_MCU)의 메모리 사용량을 출력하고,
 * 작은 코퍼스로 학습→압축→복원하여 무손실과 압축비를 확인한다.
 */
#include "ce_compress.h"
#include "btv3.h"
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

static void row(const char *name, unsigned long bytes) {
    printf("  %-16s %10lu B  (%6.2f MB)\n", name, bytes, bytes/(1024.0*1024.0));
}

int main(int argc, char **argv) {
    BtV3Mem m;
    bt_v3_footprint(&m);

    printf("BCB v3 BT memory footprint  [%s]\n\n", m.is_mcu ? "MCU (-DBCB_MCU)" : "desktop");
    printf("  BtEntry=%luB ×%lu, CtxEntry=%luB ×%lu, bloom=%lu bit, LUT=%lu×2\n\n",
           m.bt_entry_sz, m.bt_pool_n, m.ctx_entry_sz, m.ctx_pool_n, m.bloom_bits, m.lut_n);
    row("bt pool",   m.bt_pool);
    row("bt slot",   m.bt_slot);
    row("ctx pool",  m.ctx_pool);
    row("ctx slot",  m.ctx_slot);
    row("bloom",     m.bloom);
    row("LUTs",      m.luts);
    printf("  %-16s %10lu B  (%6.2f MB)\n", "TOTAL", m.total, m.total/(1024.0*1024.0));

    /* 작은 round-trip (학습→압축→복원, 무손실·압축비 확인) */
    size_t train = (argc>2) ? (size_t)strtoul(argv[2],NULL,10) : 30000;
    const char *path = (argc>1) ? argv[1] : "tests/corpus/pride_and_prejudice.txt";
    size_t len=0; uint8_t *c = slurp(path, &len);
    if (!c || len < train+4096) { printf("\n[skip roundtrip: corpus]\n"); free(c); return 0; }
    const uint8_t *ex = c + train; size_t exlen = 4096;

    CecBT bt = btv3_cec_bt();
    for (size_t i=0;i<train;i++) bt.train(c[i], bt.user);
    CecEncoder *e = cec_enc_new(&bt);
    for (size_t i=0;i<exlen;i++) cec_enc_byte(e, ex[i]);
    size_t clen=0; uint8_t *comp = cec_enc_finish(e, &clen); cec_enc_free(e);

    CecBT bt2 = btv3_cec_bt();
    for (size_t i=0;i<train;i++) bt2.train(c[i], bt2.user);
    uint8_t *dec = cec_decompress(comp, clen, exlen, &bt2);
    int ok = (dec && memcmp(dec, ex, exlen)==0);

    printf("\n round-trip: train=%zuKB excerpt=%zuB -> %zuB  (%.2fx)  entries=%lu  %s\n",
           train/1000, exlen, clen, clen?(double)exlen/clen:0.0, bt_v3_entries(),
           ok ? "lossless" : "LOSSY!");
    free(dec); free(comp); free(c);
    return ok ? 0 : 1;
}
