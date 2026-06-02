/* test_nodiv.c — BCB_MCU_NO_DIV range-coder divider unit test.
 * Includes ce_compress.c (built with -DBCB_MCU_NO_DIV) to reach the static
 * inline bcb_divq64 / bcb_log2_pow2, and asserts they equal the hardware '/'
 * (oracle) over edges + 1e6 random cases. 1 mismatch == failure. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef BCB_MCU_NO_DIV
#define BCB_MCU_NO_DIV 1
#endif
#include "ce_compress.c"   /* pulls in bcb_divq64 / bcb_log2_pow2 (static inline) */

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint64_t xr(void){ rng ^= rng<<13; rng ^= rng>>7; rng ^= rng<<17; return rng; }

int main(void){
    long fails = 0, n = 0;

    /* ── constant power-of-two divisor → shift ── */
    for (unsigned k = 0; k < 32; k++) {
        uint32_t d = 1u << k;
        if (bcb_log2_pow2(d) != k) { printf("log2_pow2(%u)=%u != %u\n", d, bcb_log2_pow2(d), k); fails++; }
        for (int t = 0; t < 100000; t++) {
            uint64_t num = xr() & ((1ull<<48)-1);
            if ((num >> k) != num / d) { fails++; break; }
            n++;
        }
    }

    /* ── variable-denominator shift-subtract divider == '/' ── */
    /* explicit edges around each denominator */
    uint64_t dens[] = {1,2,3,7,255,256,16383,16384,16385,65535,65536,
                       0x7FFFFFFFull,0x80000000ull,0xFFFFFFFFull,0x100000000ull};
    for (size_t di = 0; di < sizeof dens/sizeof dens[0]; di++) {
        uint64_t den = dens[di];
        uint64_t nums[] = {0, 1, den-1, den, den+1, 2*den, den*16384,
                           (1ull<<48)-1, (1ull<<48), (1ull<<46)+den+3};
        for (size_t ni = 0; ni < sizeof nums/sizeof nums[0]; ni++) {
            uint64_t num = nums[ni];
            if (bcb_divq64(num, den, 64) != num/den) {
                printf("EDGE divq64(%llu,%llu)=%llu != %llu\n",
                       (unsigned long long)num,(unsigned long long)den,
                       (unsigned long long)bcb_divq64(num,den,64),(unsigned long long)(num/den));
                fails++;
            }
            n++;
        }
    }
    /* 1e6 random: dividend up to 2^48, divisor up to 2^32 (matches coder ranges) */
    for (long t = 0; t < 1000000; t++) {
        uint64_t num = xr() & ((1ull<<48)-1);
        uint64_t den = (xr() & 0xFFFFFFFFull) + 1;        /* 1 .. 2^32 */
        unsigned qb = (t & 1) ? 16u : 32u;                /* exercise both call widths */
        if (bcb_divq64(num, den, qb) != num/den) { fails++; if (fails<5) printf("rand divq64(%llu,%llu,%u)=%llu != %llu\n",(unsigned long long)num,(unsigned long long)den,qb,(unsigned long long)bcb_divq64(num,den,qb),(unsigned long long)(num/den)); }
        n++;
    }

    printf("test_nodiv: %ld checks, %ld mismatches : %s\n", n, fails, fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
