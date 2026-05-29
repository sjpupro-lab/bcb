/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* test_roundtrip.c — v0 baseline 무손실 검증
 *
 * 인코드/디코드 모두 동일하게 초기화된 BT에서 출발해야 한다.
 * cec_default_bt() 가 매 호출마다 bt_v4_init() 으로 전역 상태를 리셋하므로,
 * compress 직전과 decompress 직전에 각각 새 BT 를 받으면 양쪽 상태가 일치한다.
 */
#include "ce_compress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

/* data/len 를 압축 후 복원하여 원본과 일치하는지 검증 */
static void check(const char *name, const uint8_t *data, size_t len) {
    CecBT bt = cec_default_bt();
    size_t clen = 0;
    uint8_t *comp = cec_compress(data, len, &bt, &clen);
    if (!comp) { printf("  [FAIL] %s — compress returned NULL\n", name); g_fail++; return; }

    CecBT bt2 = cec_default_bt();
    uint8_t *dec = cec_decompress(comp, clen, len, &bt2);
    if (!dec) { printf("  [FAIL] %s — decompress returned NULL\n", name); g_fail++; free(comp); return; }

    int ok = (memcmp(data, dec, len) == 0);
    double ratio = clen > 0 ? (double)len / (double)clen : 0.0;
    if (ok) {
        printf("  [PASS] %-22s %6zu -> %6zu bytes  (%.2fx)\n", name, len, clen, ratio);
        g_pass++;
    } else {
        printf("  [FAIL] %-22s round-trip mismatch\n", name);
        g_fail++;
    }
    free(comp);
    free(dec);
}

static uint8_t *read_file(const char *path, size_t cap, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = (uint8_t*)malloc(cap);
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    *out_len = n;
    return buf;
}

int main(void) {
    printf("BCB v0 baseline — round-trip tests\n");

    /* 1. 빈 입력 */
    check("empty", (const uint8_t*)"", 0);

    /* 2. 단일 바이트 */
    check("single byte", (const uint8_t*)"A", 1);

    /* 3. 짧은 ASCII */
    const char *s = "the quick brown fox jumps over the lazy dog";
    check("short ascii", (const uint8_t*)s, strlen(s));

    /* 4. 256개 모든 바이트값 */
    {
        uint8_t all[256];
        for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
        check("all 256 bytes", all, 256);
    }

    /* 5. 반복 패턴 (높은 압축비 기대) */
    {
        size_t n = 4096;
        uint8_t *rep = (uint8_t*)malloc(n);
        for (size_t i = 0; i < n; i++) rep[i] = "ABCD"[i & 3];
        check("repeating pattern", rep, n);
        free(rep);
    }

    /* 6. 의사난수 (압축 불가 — 무손실만 검증) */
    {
        size_t n = 2048;
        uint8_t *rnd = (uint8_t*)malloc(n);
        unsigned int seed = 12345u;
        for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; rnd[i] = (uint8_t)(seed >> 16); }
        check("pseudo-random", rnd, n);
        free(rnd);
    }

    /* 7. 실제 책 발췌 (있으면) */
    {
        size_t len = 0;
        uint8_t *book = read_file("tests/corpus/pride_and_prejudice.txt", 4096, &len);
        if (book && len > 0) check("book excerpt 4KB", book, len);
        else printf("  [SKIP] book excerpt — corpus not found\n");
        free(book);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
