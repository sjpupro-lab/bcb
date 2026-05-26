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
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define BCBP_MAGIC   "BCBP"
#define BCBP_VERSION 3u
#define LM_SCALE     CEC_RC_SCALE    /* landmark/schema width 합 = range coder scale */
#define SCHEMA_MAX   1024            /* 최대 record_size */

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
    uint32_t schema_rec;             /* record schema: record_size (0 이면 없음) */
    uint32_t schema_pos;             /* position 수 (= record_size) */
    uint64_t off_window, off_pool, off_ctx_pool, off_ctx_slot, off_bloom;
    uint64_t off_lm_ctx, off_lm_cum; /* lm_ctx: k*n 바이트, lm_cum: k*256 uint16 */
    uint64_t off_schema_modes;       /* schema_pos 바이트 (0=byte,1=delta) */
    uint64_t off_schema_cum;         /* schema_pos*256 uint16 (자리별 width) */
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
    /* record schema (structural landmark) */
    int             schema_rec;      /* record_size (0 = 없음) */
    int             schema_pos;
    const unsigned char *schema_modes;  /* schema_pos: 0=byte,1=delta */
    const uint16_t *schema_cum;         /* schema_pos*256 widths */
};

/* save 에 포함할 landmark (caller 가 등록) */
static int             s_lm_n = 0;
static unsigned        s_lm_k = 0;
static const unsigned char *s_lm_ctx = NULL;
static const uint16_t *s_lm_cum = NULL;

void bcb_prior_set_landmarks(int n, unsigned k, const unsigned char *ctx, const uint16_t *cum) {
    s_lm_n = n; s_lm_k = k; s_lm_ctx = ctx; s_lm_cum = cum;
}

/* save 에 포함할 schema (build helper 가 등록) */
static int             s_sc_rec = 0, s_sc_pos = 0;
static const unsigned char *s_sc_modes = NULL;
static const uint16_t *s_sc_cum = NULL;
static void bcb_prior_set_schema_(int rec, int pos, const unsigned char *modes, const uint16_t *cum) {
    s_sc_rec = rec; s_sc_pos = pos; s_sc_modes = modes; s_sc_cum = cum;
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
    int      sc_rec      = s_sc_rec, sc_pos = s_sc_pos;
    uint64_t sc_modes_bytes = (uint64_t)sc_pos;
    uint64_t sc_cum_bytes   = (uint64_t)sc_pos * 256u * sizeof(uint16_t);

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
    h.schema_rec = (uint32_t)sc_rec; h.schema_pos = (uint32_t)sc_pos;

    uint64_t off = align8(sizeof(BcbpHeader));
    h.off_window   = off;                 off = align8(off + s.bt_max_depth);
    h.off_pool     = off;                 off = align8(off + pool_bytes);
    h.off_ctx_pool = off;                 off = align8(off + ctx_bytes);
    h.off_ctx_slot = off;                 off = align8(off + slot_bytes);
    h.off_bloom    = off;                 off = align8(off + bloom_bytes);
    h.off_lm_ctx   = off;                 off = align8(off + lm_ctx_bytes);
    h.off_lm_cum   = off;                 off = align8(off + lm_cum_bytes);
    h.off_schema_modes = off;             off = align8(off + sc_modes_bytes);
    h.off_schema_cum   = off;             off = align8(off + sc_cum_bytes);
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
    ok &= pad_to(f, h.off_schema_modes); if (sc_modes_bytes) ok &= (fwrite(s_sc_modes, 1, sc_modes_bytes, f) == sc_modes_bytes);
    ok &= pad_to(f, h.off_schema_cum);   if (sc_cum_bytes)   ok &= (fwrite(s_sc_cum, 1, sc_cum_bytes, f) == sc_cum_bytes);
    if (fclose(f) != 0) ok = 0;

    s_lm_n = 0; s_lm_k = 0; s_lm_ctx = NULL; s_lm_cum = NULL;   /* 1회성: 등록 해제 */
    s_sc_rec = 0; s_sc_pos = 0; s_sc_modes = NULL; s_sc_cum = NULL;
    return ok ? 0 : -1;
}

/* ── landmark 추출 (학습된 btv3 에서) ───────────────────────── */
#define LM_BACKOFF_W 64.0    /* 경험적 분포 backoff pseudo-count */
typedef struct { unsigned long idx; unsigned total; } CtxRef;
static int cmp_ctxref(const void *a, const void *b) {
    unsigned ta = ((const CtxRef *)a)->total, tb = ((const CtxRef *)b)->total;
    return ta < tb ? 1 : (ta > tb ? -1 : 0);
}
/* P[256](합 sum) 를 width[256] 정수로 양자화. 합=scale, 각 width>=1 보장. */
static void lm_quantize(const double *P, double sum, uint16_t *w, int scale) {
    int acc = 0;
    for (int b = 0; b < 256; b++) {
        double x = sum > 0 ? P[b] / sum * scale : (double)scale / 256.0;
        int v = (int)(x + 0.5);
        if (v < 1) v = 1;
        if (v > scale) v = scale;
        w[b] = (uint16_t)v; acc += v;
    }
    int diff = scale - acc;
    while (diff > 0) {
        int bi = 0; double best = -1;
        for (int b = 0; b < 256; b++) { double pr = sum > 0 ? P[b] / sum : 1.0 / 256; if (pr > best) { best = pr; bi = b; } }
        w[bi]++; diff--;
    }
    while (diff < 0) {
        int bi = -1, mx = 1;
        for (int b = 0; b < 256; b++) if (w[b] > mx) { mx = w[b]; bi = b; }
        if (bi < 0) break;
        w[bi]--; diff++;
    }
}

int bcb_prior_save_with_landmarks(const char *path, int n, unsigned k) {
    if (n < 1 || n > 24 || k == 0) return bcb_prior_save(path);

    const unsigned char *win; int wl = bt_v3_window(&win);
    unsigned char saved[32]; int saved_len = wl > 32 ? 32 : wl;
    memcpy(saved, win, (size_t)saved_len);

    unsigned long npool = bt_v3_ctx_pool_count();
    CtxRef *refs = (CtxRef *)malloc(sizeof(CtxRef) * (npool ? npool : 1));
    unsigned long nlen = 0;
    unsigned char ctx[32]; int len; unsigned total;
    for (unsigned long i = 0; i < npool; i++) {
        if (bt_v3_ctx_at(i, ctx, &len, &total, NULL) != 0) continue;
        if (len == n && total > 0) { refs[nlen].idx = i; refs[nlen].total = total; nlen++; }
    }
    qsort(refs, nlen, sizeof(CtxRef), cmp_ctxref);
    if (k > nlen) k = (unsigned)nlen;

    unsigned char *lm_ctx = (unsigned char *)malloc((size_t)k * n + 1);
    uint16_t *lm_cum = (uint16_t *)malloc((size_t)k * 256 * sizeof(uint16_t) + 1);
    unsigned freq[256]; uint32_t cum[257];
    for (unsigned r = 0; r < k; r++) {
        bt_v3_ctx_at(refs[r].idx, ctx, &len, &total, freq);
        memcpy(lm_ctx + (size_t)r * n, ctx, (size_t)n);
        bt_v3_set_window(ctx, n);
        bt_v3_distribution(cum, CEC_RC_SCALE);
        double P[256], sum = 0, tot = (double)total + LM_BACKOFF_W;
        for (int b = 0; b < 256; b++) {
            double pbt = (double)(cum[b + 1] - cum[b]) / (double)CEC_RC_SCALE;
            P[b] = (double)freq[b] / tot + LM_BACKOFF_W * pbt / tot;
            sum += P[b];
        }
        lm_quantize(P, sum, lm_cum + (size_t)r * 256, CEC_RC_SCALE);
    }
    free(refs);
    bt_v3_set_window(saved, saved_len);          /* 학습 종료 window 복원 */

    bcb_prior_set_landmarks(n, k, lm_ctx, lm_cum);
    int rc = bcb_prior_save(path);
    free(lm_ctx); free(lm_cum);
    return rc;
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
    p->schema_rec = (int)h->schema_rec; p->schema_pos = (int)h->schema_pos;
    p->schema_modes = (const unsigned char *)map + h->off_schema_modes;
    p->schema_cum   = (const uint16_t *)((const char *)map + h->off_schema_cum);
    return p;
}

int bcb_prior_record_size(const BcbPrior *p) { return p ? p->schema_rec : 0; }

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

/* ── structural (position-aware) codec ─────────────────────
 * btv3 를 쓰지 않는다 — 분포는 자리별 schema cum 으로 결정. 단일 글로벌 상태(단일 스레드). */
static const BcbPrior *g_sc_p;
static int g_sc_pos;
static unsigned char g_sc_prev[SCHEMA_MAX];   /* 직전 레코드 바이트 */
static unsigned char g_sc_cur[SCHEMA_MAX];    /* 현재 레코드 누적 */

void bcb_prior_msg_begin(void) {
    g_sc_pos = 0;
    memset(g_sc_prev, 0, sizeof g_sc_prev);
    memset(g_sc_cur, 0, sizeof g_sc_cur);
}

static void structural_dist(uint32_t *cum, uint32_t scale, void *user) {
    const BcbPrior *p = (const BcbPrior *)user;
    int pos = g_sc_pos;
    if (pos >= p->schema_pos) pos = pos % p->schema_pos;   /* 안전 (정렬 가정) */
    const uint16_t *w = p->schema_cum + (size_t)pos * 256;
    uint32_t acc = 0;
    if (p->schema_modes[pos]) {                 /* delta: prev[pos] 만큼 순환 시프트 */
        int sh = g_sc_prev[pos];
        for (int b = 0; b < 256; b++) { cum[b] = acc; acc += w[(b - sh) & 0xFF]; }
    } else {                                    /* byte */
        for (int b = 0; b < 256; b++) { cum[b] = acc; acc += w[b]; }
    }
    cum[256] = acc;
    if (acc != scale) {                         /* 안전망: 합 불일치 시 균등 */
        unsigned int u = scale / 256, a2 = 0;
        for (int b = 0; b < 256; b++) { cum[b] = a2; a2 += (u ? u : 1); }
        cum[256] = a2;
    }
}

static void structural_train(unsigned char b, void *user) {
    const BcbPrior *p = (const BcbPrior *)user;
    g_sc_cur[g_sc_pos] = b;                      /* 실제 바이트 저장 (prev 추적) */
    g_sc_pos++;
    if (g_sc_pos >= p->schema_rec) {
        memcpy(g_sc_prev, g_sc_cur, (size_t)p->schema_rec);
        g_sc_pos = 0;
    }
}

CecBT bcb_prior_cec_bt(BcbPrior *p) {
    if (p && p->schema_rec > 0 && p->schema_pos > 0 && p->schema_rec <= SCHEMA_MAX) {
        g_sc_p = p;
        bcb_prior_msg_begin();
        CecBT bt; bt.distribution = structural_dist; bt.train = structural_train; bt.user = p;
        return bt;                               /* btv3 미사용 — attach 불필요 */
    }
    bcb_prior_attach(p);
    CecBT bt = btv3_cec_bt_from_prior();
    if (p && p->lm_k) { bt.distribution = landmark_dist; bt.user = p; }
    return bt;
}

/* ── schema 빌드: corpus 를 rec 로 정렬, 자리별 byte/delta 분포 + held-out mode ── */
int bcb_prior_save_with_schema(const char *path, const unsigned char *corpus,
                               size_t corpus_len, int rec) {
    if (rec < 1 || rec > SCHEMA_MAX) return -1;
    size_t nrec = corpus_len / (size_t)rec;
    if (nrec < 2) return bcb_prior_save(path);   /* 데이터 부족 → schema 없이 */

    /* 자리별 byte/delta 빈도 (held-out: fit 80% / val 20%) */
    static double bf[SCHEMA_MAX][256], df[SCHEMA_MAX][256];
    /* SCHEMA_MAX*256*8*2 = 4MB — 정적이라 OK */
    memset(bf, 0, (size_t)rec * 256 * sizeof(double));
    memset(df, 0, (size_t)rec * 256 * sizeof(double));
    size_t nfit = nrec >= 5 ? (nrec * 4) / 5 : nrec;

    for (size_t r = 0; r < nfit; r++)
        for (int pp = 0; pp < rec; pp++) {
            unsigned char b = corpus[r*rec + pp];
            bf[pp][b] += 1.0;
            if (r > 0) df[pp][(unsigned char)(b - corpus[(r-1)*rec + pp])] += 1.0;
        }

    unsigned char *modes = (unsigned char *)calloc((size_t)rec, 1);
    uint16_t *cum = (uint16_t *)malloc((size_t)rec * 256 * sizeof(uint16_t));
    for (int pp = 0; pp < rec; pp++) {
        double totb = 0, totd = 0;
        for (int b = 0; b < 256; b++) { totb += bf[pp][b]; totd += df[pp][b]; }
        /* held-out mode 결정 */
        double vb = 0, vd = 0; size_t vstart = (nfit < nrec) ? nfit : 1, vn = 0;
        for (size_t r = vstart; r < nrec; r++) {
            unsigned char b = corpus[r*rec + pp];
            vb += -log2((bf[pp][b] + LM_BACKOFF_W/256) / (totb + LM_BACKOFF_W));
            unsigned char d = (unsigned char)(b - corpus[(r-1)*rec + pp]);
            vd += -log2((df[pp][d] + LM_BACKOFF_W/256) / (totd + LM_BACKOFF_W));
            vn++;
        }
        int use_delta = (vn > 0 && vd < vb);
        modes[pp] = (unsigned char)use_delta;
        /* val 구간을 분포에 합산 (전체 train 으로 최종 분포) */
        for (size_t r = nfit; r < nrec; r++) {
            unsigned char b = corpus[r*rec + pp];
            bf[pp][b] += 1.0;
            if (r > 0) df[pp][(unsigned char)(b - corpus[(r-1)*rec + pp])] += 1.0;
        }
        double *src = use_delta ? df[pp] : bf[pp];
        double sum = 0; for (int b = 0; b < 256; b++) sum += src[b];
        double P[256]; for (int b = 0; b < 256; b++) P[b] = src[b];
        lm_quantize(P, sum, cum + (size_t)pp * 256, CEC_RC_SCALE);
    }

    bcb_prior_set_schema_(rec, rec, modes, cum);
    int rc = bcb_prior_save(path);
    free(modes); free(cum);
    return rc;
}
