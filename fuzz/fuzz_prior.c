/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* fuzz_prior.c — fuzz the untrusted .bcb-prior parser + downstream use.
 *
 * Target: bcb_prior_from_memory() parses an attacker-controlled prior image.
 * A malformed image must be rejected (NULL) or, if accepted, must be safe to
 * use: no OOB read/write, no crash, no UB. We therefore not only parse but also
 * exercise the codec (compress/decompress a fixed buffer) with the parsed prior,
 * which is where the distribution tables (pool/ctx_slot/bloom/landmark/schema)
 * are actually dereferenced.
 *
 * Build (libFuzzer + ASan/UBSan): see fuzz/Makefile target `fuzz_prior`.
 * Replay a crash:  ./fuzz_prior path/to/crash-seed
 */
#include "bcb.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    BcbPrior *p = bcb_prior_from_memory(data, size);
    if (!p) return 0;                 /* rejected — the safe outcome */

    /* Accepted: it must now be safe to use. Exercise the distribution tables. */
    static const uint8_t msg[] =
        "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n";
    uint8_t comp[256];
    ssize_t clen = bcb_compress(p, msg, sizeof(msg) - 1, comp, sizeof(comp));
    if (clen > 0) {
        uint8_t dec[256];
        bcb_decompress(p, comp, (size_t)clen, dec, sizeof(dec));
    }
    /* Also drive the decoder directly with a fixed pseudo-payload so the
     * structural/landmark read paths run even when compress bailed out. */
    uint8_t dec2[64];
    bcb_decompress(p, msg, sizeof(msg) - 1, dec2, sizeof(dec2));

    bcb_prior_close(p);
    return 0;
}
