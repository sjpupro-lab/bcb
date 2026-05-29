/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* fuzz_helpers.h — build a *valid* .bcb-prior image in memory for fuzz harnesses
 * and seed generation. Uses the internal btv3 training path (same as
 * tools/bcb-prior-build.c) so the resulting image is bit-identical to a real
 * prior. The decompress harness fuzzes the container bytes against this trusted
 * prior; the prior harness fuzzes the image bytes themselves. */
#ifndef BCB_FUZZ_HELPERS_H
#define BCB_FUZZ_HELPERS_H

#include "ce_compress.h"
#include "btv3.h"
#include "bcb_prior.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A small but structurally representative training corpus: repeated key/value
 * lines so the BT prior accumulates real contexts, landmarks, and a populated
 * bloom filter. Mode is text-like (no record schema) to exercise the BT path. */
static const char FUZZ_TRAIN_CORPUS[] =
    "GET /index.html HTTP/1.1\r\nHost: example.com\r\nUser-Agent: bcb/0.2\r\n"
    "Accept: text/html,application/xhtml+xml\r\nAccept-Encoding: gzip, br\r\n"
    "Connection: keep-alive\r\nCache-Control: no-cache\r\n\r\n"
    "GET /style.css HTTP/1.1\r\nHost: example.com\r\nUser-Agent: bcb/0.2\r\n"
    "Accept: text/css,*/*;q=0.1\r\nAccept-Encoding: gzip, br\r\n"
    "Connection: keep-alive\r\nReferer: https://example.com/\r\n\r\n"
    "POST /api/v1/telemetry HTTP/1.1\r\nHost: api.example.com\r\n"
    "Content-Type: application/json\r\nContent-Length: 42\r\n\r\n"
    "{\"device\":\"sensor-01\",\"temp\":21,\"ok\":true}\n"
    "{\"device\":\"sensor-02\",\"temp\":22,\"ok\":true}\n"
    "{\"device\":\"sensor-03\",\"temp\":20,\"ok\":false}\n";

/* Build a valid prior image into a malloc'd buffer. Returns the buffer (caller
 * frees) and sets *out_len, or NULL on failure. Trains with landmarks so the
 * landmark section is also populated (broader coverage). */
static unsigned char *fuzz_build_prior(size_t *out_len) {
    char tmpl[] = "/tmp/bcb_fuzz_prior_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    close(fd);

    CecBT bt = btv3_cec_bt();                 /* init + reset (training BT) */
    size_t n = sizeof(FUZZ_TRAIN_CORPUS) - 1;
    /* Repeat the corpus a few times so contexts cross the bloom/landmark
     * thresholds and the pool is non-trivial. */
    for (int rep = 0; rep < 64; rep++)
        for (size_t i = 0; i < n; i++) bt.train((unsigned char)FUZZ_TRAIN_CORPUS[i], bt.user);

    if (bcb_prior_save_with_landmarks(tmpl, 4, 64) != 0) { remove(tmpl); return NULL; }

    FILE *f = fopen(tmpl, "rb");
    if (!f) { remove(tmpl); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); remove(tmpl); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); remove(tmpl); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    remove(tmpl);
    bt_v3_free();             /* release training-side global pools (saved already) */
    if (rd != (size_t)sz) { free(buf); return NULL; }
    *out_len = rd;
    return buf;
}

#endif /* BCB_FUZZ_HELPERS_H */
