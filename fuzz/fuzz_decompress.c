/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* fuzz_decompress.c — fuzz the decode path against a *trusted* prior.
 *
 * Target: bcb_decompress() / bcb_decode() must never crash, read/write OOB,
 * loop forever, or exhibit UB on arbitrary (corrupted / adversarial) container
 * bytes. It may only return a valid length or a negative BcbStatus.
 *
 * A single valid prior is built once in LLVMFuzzerInitialize (same training path
 * as tools/bcb-prior-build.c), then every fuzz input is treated as a candidate
 * compressed container.
 *
 * Build (libFuzzer + ASan/UBSan): see fuzz/Makefile target `fuzz_decompress`.
 */
#define _POSIX_C_SOURCE 200809L       /* mkstemp under -std=c99 */
#include "bcb.h"
#include "fuzz_helpers.h"
#include <stddef.h>
#include <stdint.h>

static BcbPrior *g_prior;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    size_t plen = 0;
    unsigned char *img = fuzz_build_prior(&plen);
    if (!img) { fprintf(stderr, "fuzz: failed to build prior\n"); _exit(1); }
    g_prior = bcb_prior_from_memory(img, plen);
    free(img);
    if (!g_prior) { fprintf(stderr, "fuzz: failed to open prior\n"); _exit(1); }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Output buffer is bounded; bcb_decompress refuses orig_len > capacity, so
     * we never get asked to allocate an attacker-chosen huge buffer here. */
    uint8_t out[8192];
    bcb_decompress(g_prior, data, size, out, sizeof(out));

    /* Also drive the handle-based decoder (separate codec instance state). */
    BcbDecoder *d = bcb_decoder_new(g_prior);
    if (d) {
        bcb_decode(d, data, size, out, sizeof(out));
        bcb_decoder_free(d);
    }
    return 0;
}
