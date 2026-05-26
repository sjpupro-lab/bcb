/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* bcb_prior.c — v5 prior 직렬화 + mmap 로드. */
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
#define BCBP_VERSION 1u

/* 모든 필드 고정폭·LE 가정(동일 아키텍처에서 생성·소비). 8바이트 정렬 유지. */
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t bt_entry_sz, ctx_entry_sz;
    uint32_t bt_max_depth, pres;
    uint64_t bloom_bits;
    uint64_t pool_used, ctx_used, ctx_nslots, bloom_bytes;
    int32_t  win_len;
    uint32_t _pad0;
    uint64_t off_window, off_pool, off_ctx_pool, off_ctx_slot, off_bloom;
    uint64_t file_size;
} BcbpHeader;

struct BcbPrior {
    void  *map;
    size_t map_len;
    BtV3Snapshot snap;
};

static uint64_t align8(uint64_t x){ return (x + 7u) & ~(uint64_t)7u; }

/* f 의 현재 위치에서 target 까지 0 으로 패딩. 성공 1 / 실패 0. */
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

    uint64_t off = align8(sizeof(BcbpHeader));
    h.off_window   = off;                 off = align8(off + s.bt_max_depth);
    h.off_pool     = off;                 off = align8(off + pool_bytes);
    h.off_ctx_pool = off;                 off = align8(off + ctx_bytes);
    h.off_ctx_slot = off;                 off = align8(off + slot_bytes);
    h.off_bloom    = off;                 off = align8(off + bloom_bytes);
    h.file_size    = off;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = 1;
    /* 헤더 + 정렬 패딩까지 채우며 순서대로 기록 */
    ok &= (fwrite(&h, sizeof h, 1, f) == 1);
    ok &= pad_to(f, h.off_window);   ok &= (fwrite(s.window, 1, s.bt_max_depth, f) == s.bt_max_depth);
    ok &= pad_to(f, h.off_pool);     if (pool_bytes)  ok &= (fwrite(s.pool, 1, pool_bytes, f) == pool_bytes);
    ok &= pad_to(f, h.off_ctx_pool); if (ctx_bytes)   ok &= (fwrite(s.ctx_pool, 1, ctx_bytes, f) == ctx_bytes);
    ok &= pad_to(f, h.off_ctx_slot); if (slot_bytes)  ok &= (fwrite(s.ctx_slot, 1, slot_bytes, f) == slot_bytes);
    ok &= pad_to(f, h.off_bloom);    if (bloom_bytes) ok &= (fwrite(s.bloom, 1, bloom_bytes, f) == bloom_bytes);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
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
    madvise(map, len, MADV_RANDOM);   /* read-ahead 억제 → RSS 최소화 */

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
    return p;
}

void bcb_prior_close(BcbPrior *p) {
    if (!p) return;
    bt_v3_detach();
    if (p->map && p->map != MAP_FAILED) munmap(p->map, p->map_len);
    free(p);
}

int bcb_prior_attach(BcbPrior *p) {
    if (!p) return -1;
    return bt_v3_attach(&p->snap);
}

CecBT bcb_prior_cec_bt(BcbPrior *p) {
    bcb_prior_attach(p);
    return btv3_cec_bt_from_prior();
}
