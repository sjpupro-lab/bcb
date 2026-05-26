/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* bcb_prior.c — v5 prior 직렬화 + mmap 로드 (+ landmark prior index). */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE              /* madvise / MADV_RANDOM */
#include "bcb_prior.h"
#include "btv3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define BCBP_MAGIC   "BCBP"
#define BCBP_VERSION 2u
#define LM_SCALE     CEC_RC_SCALE    /* landmark width 합 = range coder scale */

/* 모든 필드 고정폭·LE 가정(동일 아키텍처에서 생성·소비). 8바이트 정렬 유지. */
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t bt_entry_sz, ctx_entry_sz;
    uint32_t bt_max_depth, pres;
    uint64_t bloom_bits;
    uint64_t pool_used, ctx_used, ctx_nslots, bloom_bytes;
    int32_t  win_len;
    uint32_t lm_n;                   /* landmark context 길이 (0 이면 없음) */
    uint64_t lm_k;                   /* landmark 개수 */
    uint64_t off_window, off_pool, off_ctx_pool, off_ctx_slot, off_bloom;
    uint64_t off_lm_ctx, off_lm_cum; /* lm_ctx: k*n 바이트, lm_cum: k*256 uint16 */
    uint64_t file_size;
} BcbpHeader;

struct BcbPrior {
    void  *map;
    size_t map_len;
    BtV3Snapshot snap;
    /* landmark */
    int             lm_n;
    unsigned        lm_k;
    const unsigned char *lm_ctx;     /* k*lm_n */
    const uint16_t *lm_cum;          /* k*256 widths */
    int            *lm_slot;         /* RAM 해시: hash(ctx)->landmark idx, 크기 lm_nslots */
    unsigned long   lm_nslots, lm_mask;
};

/* save 에 포함할 landmark (caller 가 등록) */
static int             s_lm_n = 0;
static unsigned        s_lm_k = 0;
static const unsigned char *s_lm_ctx = NULL;
static const uint16_t *s_lm_cum = NULL;

void bcb_prior_set_landmarks(int n, unsigned k, const unsigned char *ctx, const uint16_t *cum) {
    s_lm_n = n; s_lm_k = k; s_lm_ctx = ctx; s_lm_cum = cum;
}

static unsigned hashN(const unsigned char *c, int n){ unsigned h=2166136261u; for(int i=0;i<n;i++){h^=c[i];h*=16777619u;} return h; }

static uint64_t align8(uint64_t x){ return (x + 7u) & ~(uint64_t)7u; }

static int pad_to(FILE *f, uint64_t target) {
    static const char zeros[8] = {0};
    long cur = ftell(f);
    if (cur < 0) return 0;
    while ((uint64_t)cur < target) {
        size_t n = (size_t)(target - (uint64_t)cur);
        if (n > sizeof zeros) n = sizeof zeros;
        if (fwrite(zeros, 1, n, f) != n) return 0;
        cur += (long)n;
    }
    return 1;
}

int bcb_prior_save(const char *path) {
    BtV3Snapshot s;
    bt_v3_export(&s);

    uint64_t pool_bytes  = (uint64_t)s.pool_used  * s.bt_entry_sz;
    uint64_t ctx_bytes   = (uint64_t)s.ctx_used   * s.ctx_entry_sz;
    uint64_t slot_bytes  = (uint64_t)s.ctx_nslots * sizeof(int);
    uint64_t bloom_bytes = s.bloom_bytes;
    int      lm_n        = s_lm_k ? s_lm_n : 0;
    uint64_t lm_k        = s_lm_k;
    uint64_t lm_ctx_bytes = lm_k * (uint64_t)lm_n;
    uint64_t lm_cum_bytes = lm_k * 256u * sizeof(uint16_t);

    BcbpHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, BCBP_MAGIC, 4);
    h.version = BCBP_VERSION;
    h.bt_entry_sz = s.bt_entry_sz; h.ctx_entry_sz = s.ctx_entry_sz;
    h.bt_max_depth = s.bt_max_depth; h.pres = s.pres;
    h.bloom_bits = s.bloom_bits;
    h.pool_used = s.pool_used; h.ctx_used = s.ctx_used;
    h.ctx_nslots = s.ctx_nslots; h.bloom_bytes = bloom_bytes;
    h.win_len = s.win_len;
    h.lm_n = (uint32_t)lm_n; h.lm_k = lm_k;

    uint64_t off = align8(sizeof(BcbpHeader));
    h.off_window   = off;                 off = align8(off + s.bt_max_depth);
    h.off_pool     = off;                 off = align8(off + pool_bytes);
    h.off_ctx_pool = off;                 off = align8(off + ctx_bytes);
    h.off_ctx_slot = off;                 off = align8(off + slot_bytes);
    h.off_bloom    = off;                 off = align8(off + bloom_bytes);
    h.off_lm_ctx   = off;                 off = align8(off + lm_ctx_bytes);
    h.off_lm_cum   = off;                 off = align8(off + lm_cum_bytes);
    h.file_size    = off;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = 1;
    ok &= (fwrite(&h, sizeof h, 1, f) == 1);
    ok &= pad_to(f, h.off_window);   ok &= (fwrite(s.window, 1, s.bt_max_depth, f) == s.bt_max_depth);
    ok &= pad_to(f, h.off_pool);     if (pool_bytes)  ok &= (fwrite(s.pool, 1, pool_bytes, f) == pool_bytes);
    ok &= pad_to(f, h.off_ctx_pool); if (ctx_bytes)   ok &= (fwrite(s.ctx_pool, 1, ctx_bytes, f) == ctx_bytes);
    ok &= pad_to(f, h.off_ctx_slot); if (slot_bytes)  ok &= (fwrite(s.ctx_slot, 1, slot_bytes, f) == slot_bytes);
    ok &= pad_to(f, h.off_bloom);    if (bloom_bytes) ok &= (fwrite(s.bloom, 1, bloom_bytes, f) == bloom_bytes);
    ok &= pad_to(f, h.off_lm_ctx);   if (lm_ctx_bytes) ok &= (fwrite(s_lm_ctx, 1, lm_ctx_bytes, f) == lm_ctx_bytes);
    ok &= pad_to(f, h.off_lm_cum);   if (lm_cum_bytes) ok &= (fwrite(s_lm_cum, 1, lm_cum_bytes, f) == lm_cum_bytes);
    if (fclose(f) != 0) ok = 0;

    s_lm_n = 0; s_lm_k = 0; s_lm_ctx = NULL; s_lm_cum = NULL;   /* 1회성: 등록 해제 */
    return ok ? 0 : -1;
}

static void lm_build_index(BcbPrior *p) {
    if (!p->lm_k) { p->lm_slot = NULL; p->lm_nslots = 0; p->lm_mask = 0; return; }
    unsigned long ns = 1; while (ns < (unsigned long)p->lm_k * 2) ns <<= 1;
    p->lm_slot = (int *)malloc(sizeof(int) * ns);
    p->lm_nslots = ns; p->lm_mask = ns - 1;
    for (unsigned long i = 0; i < ns; i++) p->lm_slot[i] = -1;
    for (unsigned k = 0; k < p->lm_k; k++) {
        const unsigned char *c = p->lm_ctx + (size_t)k * p->lm_n;
        unsigned long h = hashN(c, p->lm_n) & p->lm_mask;
        while (p->lm_slot[h] >= 0) h = (h + 1) & p->lm_mask;
        p->lm_slot[h] = (int)k;
    }
}

static int lm_lookup(const BcbPrior *p, const unsigned char *ctx) {
    if (!p->lm_k) return -1;
    unsigned long h = hashN(ctx, p->lm_n) & p->lm_mask;
    for (;;) {
        int s = p->lm_slot[h];
        if (s < 0) return -1;
        if (memcmp(p->lm_ctx + (size_t)s * p->lm_n, ctx, (size_t)p->lm_n) == 0) return s;
        h = (h + 1) & p->lm_mask;
    }
}

BcbPrior *bcb_prior_mmap(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(BcbpHeader)) { close(fd); return NULL; }
    size_t len = (size_t)st.st_size;
    void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return NULL;
    madvise(map, len, MADV_RANDOM);

    const BcbpHeader *h = (const BcbpHeader *)map;
    if (memcmp(h->magic, BCBP_MAGIC, 4) != 0 || h->version != BCBP_VERSION ||
        h->file_size > len) { munmap(map, len); return NULL; }

    BcbPrior *p = (BcbPrior *)calloc(1, sizeof *p);
    if (!p) { munmap(map, len); return NULL; }
    p->map = map; p->map_len = len;
    p->snap.pool        = (const char *)map + h->off_pool;     p->snap.pool_used  = h->pool_used;
    p->snap.ctx_pool    = (const char *)map + h->off_ctx_pool; p->snap.ctx_used   = h->ctx_used;
    p->snap.ctx_slot    = (const char *)map + h->off_ctx_slot; p->snap.ctx_nslots = h->ctx_nslots;
    p->snap.bloom       = (const char *)map + h->off_bloom;    p->snap.bloom_bytes= h->bloom_bytes;
    p->snap.window      = (const unsigned char *)map + h->off_window; p->snap.win_len = h->win_len;
    p->snap.bt_entry_sz = h->bt_entry_sz; p->snap.ctx_entry_sz = h->ctx_entry_sz;
    p->snap.bt_max_depth = h->bt_max_depth; p->snap.bloom_bits = (unsigned)h->bloom_bits;
    p->snap.pres = h->pres;
    p->lm_n = (int)h->lm_n; p->lm_k = (unsigned)h->lm_k;
    p->lm_ctx = (const unsigned char *)map + h->off_lm_ctx;
    p->lm_cum = (const uint16_t *)((const char *)map + h->off_lm_cum);
    lm_build_index(p);
    return p;
}

void bcb_prior_close(BcbPrior *p) {
    if (!p) return;
    bt_v3_detach();
    free(p->lm_slot);
    if (p->map && p->map != MAP_FAILED) munmap(p->map, p->map_len);
    free(p);
}

int bcb_prior_attach(BcbPrior *p) {
    if (!p) return -1;
    return bt_v3_attach(&p->snap);
}

/* landmark hit 시 저장된 정수 width 로 cum 구성, miss 면 BT blend. enc/dec 동일. */
static void landmark_dist(uint32_t *cum, uint32_t scale, void *user) {
    BcbPrior *p = (BcbPrior *)user;
    const unsigned char *win;
    int wl = bt_v3_window(&win);
    if (p && p->lm_k && scale == (uint32_t)LM_SCALE && wl >= p->lm_n) {
        int idx = lm_lookup(p, win + (wl - p->lm_n));
        if (idx >= 0) {
            const uint16_t *w = p->lm_cum + (size_t)idx * 256;
            uint32_t acc = 0;
            for (int b = 0; b < 256; b++) { cum[b] = acc; acc += w[b]; }
            cum[256] = acc;
            if (acc == scale) return;        /* 유효(합=scale)할 때만 사용 */
        }
    }
    bt_v3_distribution(cum, scale);
}

CecBT bcb_prior_cec_bt(BcbPrior *p) {
    bcb_prior_attach(p);
    CecBT bt = btv3_cec_bt_from_prior();
    if (p && p->lm_k) { bt.distribution = landmark_dist; bt.user = p; }
    return bt;
}
