/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* gen_seeds.c — generate seed corpora for the fuzz harnesses.
 *   seeds_prior/valid.bcb-prior     : a valid prior image (fuzz_prior seed)
 *   seeds_decompress/c_*.bin        : valid compressed containers (fuzz_decompress seeds)
 * Run from the fuzz/ directory: `make seeds`. */
#define _POSIX_C_SOURCE 200809L       /* mkstemp under -std=c99 */
#include "bcb.h"
#include "fuzz_helpers.h"

static void dump(const char *path, const unsigned char *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fwrite(d, 1, n, f); fclose(f);
    fprintf(stderr, "wrote %s (%zu bytes)\n", path, n);
}

int main(void) {
    size_t plen = 0;
    unsigned char *img = fuzz_build_prior(&plen);
    if (!img) { fprintf(stderr, "build prior failed\n"); return 1; }
    dump("seeds_prior/valid.bcb-prior", img, plen);

    BcbPrior *p = bcb_prior_from_memory(img, plen);
    if (!p) { free(img); fprintf(stderr, "open prior failed\n"); return 1; }

    static const char *msgs[] = {
        "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n",
        "{\"device\":\"sensor-01\",\"temp\":21,\"ok\":true}\n",
        "POST /api/v1/telemetry HTTP/1.1\r\nContent-Length: 42\r\n\r\n",
        "",
    };
    char name[64];
    for (size_t i = 0; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
        size_t mlen = 0; const char *m = msgs[i]; while (m[mlen]) mlen++;
        uint8_t comp[512];
        ssize_t clen = bcb_compress(p, (const uint8_t *)m, mlen, comp, sizeof(comp));
        if (clen <= 0) continue;
        snprintf(name, sizeof(name), "seeds_decompress/c_%zu.bin", i);
        dump(name, comp, (size_t)clen);
    }
    bcb_prior_close(p);
    free(img);
    return 0;
}
