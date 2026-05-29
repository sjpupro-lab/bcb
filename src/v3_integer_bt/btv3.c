/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* btv3.c — v3 BT
 *
 * 단계 1: distribution caching (활성 context 1회 탐색 + per-context next-byte 순회).
 * 단계 2: open addressing + bloom 4M→16M (대규모 학습 O(n²)→O(1)).
 * 단계 3: 정수 전용 hot path (log-domain + max-normalization).
 *   w = exp(n)·conf²⁰ 는 conf²⁰ 가 uint64 를 넘는 극단 동적범위 → 직접 계산 불가.
 *   대신 log2 영역에서:  log2_w = EXP_LOG2[n] + CONF_LOG2[p]   (둘 다 Q16 정수 LUT)
 *   레벨별 max_log2 로 정규화:  w_int = EXP2(log2_w − max_log2)  (∈ (0, WSCALE], 정수)
 *   비율(lws/lwt)에서 정규화 상수가 상쇄되어 overflow 없이 동일 분포를 얻는다.
 *   분포 계산 hot path 에 double 없음. LUT 는 init 에서 1회 생성(step4 에서 const 베이크).
 *
 * 후속: aux 정수화 + MCU 빌드(4).
 */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "btv3.h"
/* 단계 4: libm 제거 — LUT 는 정수 fixed-point 로 생성 (no <math.h>). */

/* ── 크기 설정 — 3 모드 ─────────────────────────────────────
 *   MCU(-DBCB_MCU)      : 소형 고정, 성장 없음 (ESP32·RP2040).
 *   고정(-DBCB_POOL_BITS): 2^N 고정, 성장 없음 (pool 확장 1 비교용).
 *   기본(매크로 없음)    : 동적 — 작게 시작해 realloc 으로 성장(상한 없음).
 * POOL_CAP0/…0 은 시작(또는 고정) 용량. */
#ifdef BCB_MCU
  #define BTV3_DYNAMIC 0
  #define POOL_CAP0   (64*1024UL)
  #define BT_NSLOTS0  (1UL<<17)         /* 128K (load ≤0.5) */
  #define CTX_CAP0    (16*1024UL)
  #define CTX_NSLOTS0 (1UL<<15)         /* 32K */
  #define BLOOM_BITS  (1<<18)           /* 256K bit */
  #define PRES_BITS   12
  #define EXP2_FB     12
#elif defined(BCB_POOL_BITS)
  #define BTV3_DYNAMIC 0
  #define POOL_CAP0   (1UL<<BCB_POOL_BITS)
  #define BT_NSLOTS0  (1UL<<(BCB_POOL_BITS+1))
  #define CTX_CAP0    (1UL<<(BCB_POOL_BITS-1))
  #define CTX_NSLOTS0 (1UL<<BCB_POOL_BITS)
  #define BLOOM_BITS  (1<<24)
  #define PRES_BITS   16
  #define EXP2_FB     16
#else
  #define BTV3_DYNAMIC 1
  #define POOL_CAP0   (1UL<<16)         /* 64K 시작 */
  #define BT_NSLOTS0  (1UL<<17)
  #define CTX_CAP0    (1UL<<15)
  #define CTX_NSLOTS0 (1UL<<16)
  #define BLOOM_BITS  (1<<24)
  #define PRES_BITS   16
  #define EXP2_FB     16
#endif
#define BT_MAX_DEPTH  24
#define BT_LEVELS_V4  6
#define BLOOM_MASK    (BLOOM_BITS-1)

/* ── 고정소수 파라미터 ───────────────────────────────────── */
#define LOG2_FB    16                 /* log2 값 Q16 */
#define PRES       (1<<PRES_BITS)     /* p 양자화 해상도 (CONF_LOG2 인덱스) */
#define WBITS      28                 /* 가중치 스케일 Q28 */
#define WSCALE     ((int64_t)1<<WBITS)
#define PBITS      28                 /* 확률 스케일 Q28 */
#define PSCALE     ((int64_t)1<<PBITS)
#define EXP2_FN    (1<<EXP2_FB)       /* EXP2 분수부 해상도 */
#define CONF_EXP   20                 /* conf 지수 */

typedef struct { unsigned char ctx[BT_MAX_DEPTH]; unsigned char ctx_len, next_byte; unsigned int freq; int ctx_next; } BtEntry;
typedef struct { unsigned char ctx[BT_MAX_DEPTH]; unsigned char ctx_len; unsigned int total_freq, unique_next; int first_entry; } CtxEntry;

static CtxEntry *g_ctx_pool;
static int      *g_ctx_slot;
static unsigned long g_ctx_used, g_ctx_cap, g_ctx_nslots, g_ctx_mask;
static BtEntry  *g_pool;
static int      *g_bt_slot;
static unsigned long g_pool_used, g_pool_cap, g_bt_nslots, g_bt_mask;
static unsigned char *g_bloom;        /* BLOOM_BITS/8 바이트 (init 할당 또는 mmap attach) */
static unsigned char g_window[BT_MAX_DEPTH];
static int g_win_len;
static int g_frozen   = 0;            /* 1 이면 train 이 pool 미수정(window 만 전진) */
static int g_attached = 0;            /* 1 이면 pool/ctx/slot/bloom 이 외부(mmap) 소유 */

/* LUT (init 에서 생성) */
static int32_t EXP_LOG2[BT_MAX_DEPTH+1];   /* Q16: log2(exp(n)) */
static int32_t *CONF_LOG2;                 /* [PRES] Q16: 20·log2(conf(p)) */
static int32_t *EXP2_FRAC;                 /* [EXP2_FN] WSCALE·2^(-frac) */

typedef struct { int dmin,dmax; int wint; } BtLevel;   /* wint = round(weight*256) */
static BtLevel LEVELS[BT_LEVELS_V4] = {
    {1,2,230},{3,4,333},{5,8,384},{9,12,435},{13,16,384},{17,24,282}
};

static inline unsigned int fnv(const unsigned char *d, int n){unsigned int h=2166136261u;for(int i=0;i<n;i++){h^=d[i];h*=16777619u;}return h;}

/* 2^(norm_q16), norm<=0 → (0, WSCALE] 정수. EXP2_FB(≤LOG2_FB) 해상도로 분수부 인덱싱. */
static inline int64_t exp2_w(int64_t norm_q16){
    int64_t neg = -norm_q16;                 /* >=0, Q16 */
    int ip = (int)(neg >> LOG2_FB);
    if (ip >= 40) return 0;
    int fr = (int)((neg >> (LOG2_FB - EXP2_FB)) & (EXP2_FN-1));
    return (int64_t)EXP2_FRAC[fr] >> ip;
}

/* ── 동적 성장 (pool·slot 2배 + slot rehash). 반환 0 ok / -1 불가(고정·MCU 또는 OOM) ── */
static int bt_grow(void){
    if(!BTV3_DYNAMIC) return -1;
    unsigned long ncap = g_pool_cap*2, ns = g_bt_nslots*2;
    BtEntry *np = (BtEntry*)realloc(g_pool, sizeof(BtEntry)*ncap); if(!np) return -1; g_pool=np; g_pool_cap=ncap;
    int *nslot = (int*)realloc(g_bt_slot, sizeof(int)*ns); if(!nslot) return -1; g_bt_slot=nslot;
    g_bt_nslots=ns; g_bt_mask=ns-1;
    memset(g_bt_slot, -1, sizeof(int)*ns);
    for(unsigned long i=0;i<g_pool_used;i++){               /* rehash: index 는 불변 */
        BtEntry *e=&g_pool[i];
        unsigned int h=((fnv(e->ctx,e->ctx_len)^e->next_byte)*16777619u)&g_bt_mask;
        while(g_bt_slot[h]>=0) h=(h+1)&g_bt_mask;
        g_bt_slot[h]=(int)i;
    }
    return 0;
}
static int ctx_grow(void){
    if(!BTV3_DYNAMIC) return -1;
    unsigned long ncap = g_ctx_cap*2, ns = g_ctx_nslots*2;
    CtxEntry *np = (CtxEntry*)realloc(g_ctx_pool, sizeof(CtxEntry)*ncap); if(!np) return -1; g_ctx_pool=np; g_ctx_cap=ncap;
    int *nslot = (int*)realloc(g_ctx_slot, sizeof(int)*ns); if(!nslot) return -1; g_ctx_slot=nslot;
    g_ctx_nslots=ns; g_ctx_mask=ns-1;
    memset(g_ctx_slot, -1, sizeof(int)*ns);
    for(unsigned long i=0;i<g_ctx_used;i++){
        CtxEntry *e=&g_ctx_pool[i];
        unsigned int h=fnv(e->ctx,e->ctx_len)&g_ctx_mask;
        while(g_ctx_slot[h]>=0) h=(h+1)&g_ctx_mask;
        g_ctx_slot[h]=(int)i;
    }
    return 0;
}

/* ── CtxEntry open addressing ─────────────────────────────── */
static inline CtxEntry* ctx_find_h(const unsigned char *ctx, int cl, unsigned int ch){
    unsigned int h = ch & g_ctx_mask;
    for(;;){
        int s = g_ctx_slot[h];
        if(s < 0) return NULL;
        CtxEntry *e = &g_ctx_pool[s];
        if(e->ctx_len==cl && memcmp(e->ctx,ctx,cl)==0) return e;
        h = (h+1) & g_ctx_mask;
    }
}
static inline CtxEntry* ctx_get_h(const unsigned char *ctx, int cl, unsigned int ch){
    for(;;){
        unsigned int h = ch & g_ctx_mask;
        for(;;){
            int s = g_ctx_slot[h];
            if(s < 0){
                if(g_ctx_used >= g_ctx_cap){ if(ctx_grow()!=0) return NULL; break; } /* 성장 후 재탐사 */
                int idx = (int)g_ctx_used;
                CtxEntry *e = &g_ctx_pool[idx];
                memcpy(e->ctx,ctx,cl); e->ctx_len=cl; e->total_freq=0; e->unique_next=0; e->first_entry=-1;
                g_ctx_slot[h] = idx; g_ctx_used++;
                return e;
            }
            CtxEntry *e = &g_ctx_pool[s];
            if(e->ctx_len==cl && memcmp(e->ctx,ctx,cl)==0) return e;
            h = (h+1) & g_ctx_mask;
        }
    }
}

static inline void bloom_set(const unsigned char *ctx, int cl){
    unsigned int ch=fnv(ctx,cl);
    g_bloom[(ch&BLOOM_MASK)>>3]|=(1<<(ch&7));
    unsigned int ch2=ch*2654435761u;
    g_bloom[(ch2&BLOOM_MASK)>>3]|=(1<<(ch2&7));
}
static inline int bloom_chk(const unsigned char *ctx, int cl){
    unsigned int ch=fnv(ctx,cl);
    if(!(g_bloom[(ch&BLOOM_MASK)>>3]&(1<<(ch&7))))return 0;
    unsigned int ch2=ch*2654435761u;
    if(!(g_bloom[(ch2&BLOOM_MASK)>>3]&(1<<(ch2&7))))return 0;
    return 1;
}

static void bt_update(const unsigned char *ctx, int cl, unsigned char next){
    if(cl<=0||cl>BT_MAX_DEPTH)return;
    unsigned int ch=fnv(ctx,cl);
    bloom_set(ctx,cl);
    unsigned int key = (ch ^ next) * 16777619u;
    int is_new; long new_idx = -1;
    for(;;){                                    /* 바깥: 성장 시 재탐사 */
        unsigned int h = key & g_bt_mask;
        int done = 0;
        for(;;){
            int s = g_bt_slot[h];
            if(s < 0){
                if(g_pool_used >= g_pool_cap){ if(bt_grow()!=0) return; break; } /* 재탐사 */
                new_idx = (long)g_pool_used;
                BtEntry *e = &g_pool[new_idx];
                memcpy(e->ctx,ctx,cl); e->ctx_len=cl; e->next_byte=next; e->freq=1; e->ctx_next=-1;
                g_bt_slot[h] = (int)new_idx; g_pool_used++;
                is_new = 1; done = 1; break;
            }
            BtEntry *e = &g_pool[s];
            if(e->ctx_len==cl && e->next_byte==next && memcmp(e->ctx,ctx,cl)==0){ e->freq++; is_new=0; done=1; break; }
            h = (h+1) & g_bt_mask;
        }
        if(done) break;
    }
    CtxEntry *cc = ctx_get_h(ctx,cl,ch);
    if(cc){
        cc->total_freq++;
        if(is_new){
            cc->unique_next++;
            if(new_idx>=0){ g_pool[new_idx].ctx_next = cc->first_entry; cc->first_entry = (int)new_idx; }
        }
    }
}

/* 정수 sqrt (uint64) */
static uint64_t isqrt64(uint64_t x){
    uint64_t r=0, bit=1ULL<<62;
    while(bit>x) bit>>=2;
    while(bit){ if(x>=r+bit){ x-=r+bit; r=(r>>1)+bit; } else r>>=1; bit>>=2; }
    return r;
}
/* log2(real)·2^16, 입력 x 는 Q16 (x=real·2^16, real>=1). libm 없이 정수만. */
static int32_t log2_q16(uint32_t x){
    int msb=0; uint32_t t=x; while(t>1){t>>=1;msb++;}   /* floor(log2(x)) */
    int ip = msb - 16;                                  /* real 정수부 */
    int32_t res = ip;                                   /* loop 가 16회 <<1 하며 Q16 완성 */
    uint64_t m = (ip>=0) ? ((uint64_t)x >> ip) : ((uint64_t)x << (-ip)); /* mantissa∈[2^16,2^17) */
    for(int i=0;i<16;i++){
        m = (m*m) >> 16;
        res <<= 1;
        if(m >= (1u<<17)){ m >>= 1; res |= 1; }
    }
    return res;
}
static void build_luts(void){
    if(!CONF_LOG2)  CONF_LOG2  = (int32_t*)malloc(sizeof(int32_t)*PRES);
    if(!EXP2_FRAC)  EXP2_FRAC  = (int32_t*)malloc(sizeof(int32_t)*EXP2_FN);

    /* EXP_LOG2[n] = n·log2(e)·2^16, log2(e)·2^16 = round(1.4426950409·65536) = 94548 */
    for(int n=0;n<=BT_MAX_DEPTH;n++) EXP_LOG2[n] = (int32_t)n * 94548;

    /* CONF_LOG2[i] = 20·log2(conf)·2^16, conf = (i+0.5)/PRES·256, clamp≥1.
     * conf 의 Q16 표현: conf_q16 = (i+0.5)/PRES·256·2^16 = (2i+1)·128·2^16/PRES.  */
    for(int i=0;i<PRES;i++){
        uint64_t conf_q16 = ((uint64_t)(2*i+1) * 128ULL * (1u<<LOG2_FB)) / PRES;
        if(conf_q16 < (1u<<LOG2_FB)) conf_q16 = (1u<<LOG2_FB);   /* clamp conf≥1 */
        CONF_LOG2[i] = (int32_t)(CONF_EXP * log2_q16((uint32_t)conf_q16));
    }

    /* EXP2_FRAC[f] = 2^(-f/EXP2_FN)·WSCALE.  TWO_POW[k]=2^(2^-k) 를 정수 sqrt 로 생성. */
    uint64_t two_pow[17];
    two_pow[0] = (uint64_t)2 << LOG2_FB;                 /* 2^1 in Q16 */
    for(int k=1;k<=16;k++) two_pow[k] = isqrt64(two_pow[k-1] << LOG2_FB);
    for(int f=0;f<EXP2_FN;f++){
        /* frac_q16 = f·2^16/EXP2_FN. e2 = 2^frac in Q16 via bit 분해 */
        uint32_t frac_q16 = (uint32_t)(((uint64_t)f << LOG2_FB) / EXP2_FN);
        uint64_t e2 = 1u << LOG2_FB;
        for(int k=1;k<=16;k++) if(frac_q16 & (1u<<(LOG2_FB-k))) e2 = (e2*two_pow[k]) >> LOG2_FB;
        EXP2_FRAC[f] = (int32_t)(((uint64_t)WSCALE << LOG2_FB) / e2);   /* WSCALE·2^(-frac) */
    }
}

void bt_v3_init(void){
    /* attach 상태였다면 외부(mmap) 포인터를 버리고 새로 할당받는다 (munmap 은 호출자 책임). */
    if(g_attached){ g_pool=NULL; g_ctx_pool=NULL; g_ctx_slot=NULL; g_bt_slot=NULL; g_bloom=NULL; g_attached=0; }
    g_frozen=0;
    /* 시작(또는 고정) 용량으로 (재)할당. 동적 모드에선 이전 실행에서 커진 분을 초기로 되돌린다. */
    g_pool_cap=POOL_CAP0; g_bt_nslots=BT_NSLOTS0; g_bt_mask=g_bt_nslots-1;
    g_ctx_cap=CTX_CAP0;  g_ctx_nslots=CTX_NSLOTS0; g_ctx_mask=g_ctx_nslots-1;
    g_pool     = (BtEntry*)realloc(g_pool,     sizeof(BtEntry)*g_pool_cap);
    g_ctx_pool = (CtxEntry*)realloc(g_ctx_pool, sizeof(CtxEntry)*g_ctx_cap);
    g_bt_slot  = (int*)realloc(g_bt_slot,  sizeof(int)*g_bt_nslots);
    g_ctx_slot = (int*)realloc(g_ctx_slot, sizeof(int)*g_ctx_nslots);
    if(!g_bloom) g_bloom = (unsigned char*)malloc(BLOOM_BITS/8);
    build_luts();
    memset(g_bt_slot,  -1, sizeof(int)*g_bt_nslots);
    memset(g_ctx_slot, -1, sizeof(int)*g_ctx_nslots);
    memset(g_bloom, 0, BLOOM_BITS/8);
    memset(g_window, 0, sizeof(g_window));
    g_win_len=0; g_ctx_used=0; g_pool_used=0;
}
void bt_v3_free(void){
    if(!g_attached){
        free(g_pool); free(g_ctx_pool); free(g_ctx_slot); free(g_bloom);
    }
    g_pool=NULL; g_ctx_pool=NULL; g_ctx_slot=NULL; g_bloom=NULL;
    free(g_bt_slot); g_bt_slot=NULL;          /* attach 시 미사용·NULL → free(NULL) 안전 */
    free(CONF_LOG2); CONF_LOG2=NULL;
    free(EXP2_FRAC); EXP2_FRAC=NULL;
    g_attached=0; g_frozen=0;
}
void bt_v3_train(unsigned char b){
    if(!g_frozen)
        for(int n=1;n<=g_win_len&&n<=BT_MAX_DEPTH;n++)
            bt_update(g_window+(g_win_len-n),n,b);
    if(g_win_len<BT_MAX_DEPTH) g_window[g_win_len++]=b;
    else{memmove(g_window,g_window+1,BT_MAX_DEPTH-1);g_window[BT_MAX_DEPTH-1]=b;}
}
void bt_v3_freeze(int on){ g_frozen = on?1:0; }
void bt_v3_reset_window(void){ memset(g_window,0,sizeof(g_window)); g_win_len=0; }
void bt_v3_set_window(const unsigned char *ctx, int len){
    if(len<0) len=0;
    if(len>BT_MAX_DEPTH) len=BT_MAX_DEPTH;
    memcpy(g_window, ctx, (size_t)len);
    g_win_len=len;
}
int bt_v3_window(const unsigned char **out){ if(out) *out=g_window; return g_win_len; }

unsigned long bt_v3_ctx_pool_count(void){ return g_ctx_used; }
int bt_v3_ctx_at(unsigned long i, unsigned char *ctx_out, int *len_out,
                 unsigned *total_out, unsigned *freq256){
    if(i>=g_ctx_used) return -1;
    CtxEntry *cc=&g_ctx_pool[i];
    if(ctx_out) memcpy(ctx_out, cc->ctx, cc->ctx_len);
    if(len_out) *len_out=cc->ctx_len;
    if(total_out) *total_out=cc->total_freq;
    if(freq256){
        memset(freq256,0,256*sizeof(unsigned));
        for(int idx=cc->first_entry; idx>=0; idx=g_pool[idx].ctx_next)
            freq256[g_pool[idx].next_byte]=g_pool[idx].freq;
    }
    return 0;
}

/* 정수 전용 분포 계산. 레벨마다:
 *   pass A — 활성 context 수집 + log2_w 최대값(max_log2) 탐색
 *   pass B — w=EXP2(log2_w−max) 로 base(미관측)+delta(관측) 누적 → byte별 prob_lv
 * 레벨 가중 결합 → 정규화·alpha-blend·scale 양자화. */
/* reader 용 lookup (전역 미사용) */
static inline const CtxEntry* ctx_find_rd(const BtV3Reader *r, const unsigned char *ctx, int cl, unsigned int ch){
    const CtxEntry *cpool = (const CtxEntry*)r->ctx_pool;
    unsigned int h = ch & r->ctx_mask;
    for(;;){
        int s = r->ctx_slot[h];
        if(s < 0) return NULL;
        const CtxEntry *e = &cpool[s];
        if(e->ctx_len==cl && memcmp(e->ctx,ctx,cl)==0) return e;
        h = (h+1) & r->ctx_mask;
    }
}
static inline int bloom_chk_rd(const unsigned char *bloom, const unsigned char *ctx, int cl){
    unsigned int ch=fnv(ctx,cl);
    if(!(bloom[(ch&BLOOM_MASK)>>3]&(1<<(ch&7))))return 0;
    unsigned int ch2=ch*2654435761u;
    if(!(bloom[(ch2&BLOOM_MASK)>>3]&(1<<(ch2&7))))return 0;
    return 1;
}

/* 정수 분포 계산 (canonical). 전역 미사용 — reader 로부터만 읽는다. 스택 로컬(thread-safe). */
void bt_v3_distribution_r(const BtV3Reader *r, unsigned int *cum_out, unsigned int scale) {
    int64_t probs[256];
    const unsigned char *win = r->window; int wl = r->win_len;
    const BtEntry *pool = (const BtEntry*)r->pool;

    if (wl == 0) {
        for (int b=0;b<256;b++) probs[b] = PSCALE/256;
    } else {
        int64_t ws[256], wt[256];
        for (int b=0;b<256;b++){ ws[b]=0; wt[b]=0; }
        int      act_n[BT_MAX_DEPTH];
        unsigned act_total[BT_MAX_DEPTH];
        int64_t  act_l0[BT_MAX_DEPTH], act_p0[BT_MAX_DEPTH];
        unsigned act_freq[BT_MAX_DEPTH][256];     /* ~24KB stack, thread-local */

        for (int lv=0; lv<BT_LEVELS_V4; lv++) {
            int dmin=LEVELS[lv].dmin, dmax=LEVELS[lv].dmax;
            int start = wl<dmax?wl:dmax;
            int nact=0;
            for (int n=start; n>=dmin; n--) {
                if (n>wl) continue;
                const unsigned char *ctx = win + (wl-n);
                if (!bloom_chk_rd(r->bloom,ctx,n)) continue;
                unsigned int ch = fnv(ctx,n);
                const CtxEntry *cc = ctx_find_rd(r,ctx,n,ch);
                if (!cc || cc->total_freq==0) continue;
                unsigned total = cc->total_freq;
                int a = nact++;
                act_n[a]=n; act_total[a]=total;
                int idx0 = (int)(PRES/(int64_t)(total+256)); if(idx0>=PRES)idx0=PRES-1;
                act_l0[a] = (int64_t)EXP_LOG2[n] + CONF_LOG2[idx0];
                act_p0[a] = PSCALE/(int64_t)(total+256);
                memset(act_freq[a], 0, 256*sizeof(unsigned));
                for (int idx=cc->first_entry; idx>=0; idx=pool[idx].ctx_next)
                    act_freq[a][pool[idx].next_byte] = pool[idx].freq;
            }
            if (nact==0) continue;
            int W = LEVELS[lv].wint;
            for (int b=0;b<256;b++) {
                int64_t tl[BT_MAX_DEPTH], tp[BT_MAX_DEPTH];
                int64_t max_l = INT64_MIN;
                for (int a=0; a<nact; a++) {
                    unsigned f = act_freq[a][b];
                    int64_t l, p;
                    if (f>0) {
                        unsigned total = act_total[a];
                        int ii = (int)(((int64_t)f*PRES)/total); if(ii>=PRES)ii=PRES-1;
                        l = (int64_t)EXP_LOG2[act_n[a]] + CONF_LOG2[ii];
                        p = ((int64_t)f*PSCALE)/total;
                    } else { l = act_l0[a]; p = act_p0[a]; }
                    tl[a]=l; tp[a]=p; if(l>max_l) max_l=l;
                }
                int64_t num=0, den=0;
                for (int a=0; a<nact; a++) {
                    int64_t w = exp2_w(tl[a] - max_l);
                    num += w*tp[a]; den += w;
                }
                if (den>0) { int64_t prob_lv = num/den; ws[b] += (int64_t)W*prob_lv; wt[b] += (int64_t)W; }
            }
        }
        for (int b=0;b<256;b++) {
            int64_t prob = wt[b]>0 ? ws[b]/wt[b] : (PSCALE/256);
            if (prob<1) prob=1;
            if (prob>PSCALE) prob=PSCALE;
            probs[b]=prob;
        }
    }
    int64_t s = 0;
    for (int b=0;b<256;b++) s += probs[b];
    if (s <= 0) { unsigned int w=scale/256; for(int b=0;b<256;b++)cum_out[b]=b*w; cum_out[256]=scale; return; }
    int64_t base_w = (50*(int64_t)scale)/(1000*256);
    unsigned int acc = 0;
    for (int b=0;b<256;b++) {
        int64_t w = (950*(int64_t)scale*probs[b])/(1000*s) + base_w;
        if (w<1) w=1;
        cum_out[b] = acc; acc += (unsigned int)w;
    }
    cum_out[256] = acc;
}

/* 전역 wrapper: 현재 globals 로 reader 를 만들어 canonical 경로로 위임 (bit-identical). */
void bt_v3_distribution(unsigned int *cum_out, unsigned int scale) {
    BtV3Reader r;
    r.pool=g_pool; r.ctx_pool=g_ctx_pool; r.ctx_slot=g_ctx_slot;
    r.bloom=g_bloom; r.ctx_mask=g_ctx_mask;
    int wl = g_win_len > BT_MAX_DEPTH ? BT_MAX_DEPTH : g_win_len;
    memcpy(r.window, g_window, (size_t)wl); r.win_len = wl;
    bt_v3_distribution_r(&r, cum_out, scale);
}

void bt_v3_ensure_luts(void){ if(!CONF_LOG2 || !EXP2_FRAC) build_luts(); }
void bt_v3_reader_reset_window(BtV3Reader *r){ memset(r->window,0,sizeof(r->window)); r->win_len=0; }
void bt_v3_reader_push(BtV3Reader *r, unsigned char b){
    if(r->win_len<BT_MAX_DEPTH) r->window[r->win_len++]=b;
    else { memmove(r->window, r->window+1, BT_MAX_DEPTH-1); r->window[BT_MAX_DEPTH-1]=b; }
}
unsigned long bt_v3_entries(void){return g_pool_used;}

/* 현재(런타임) 할당 기준 footprint. init 전이면 시작 용량으로 보고. */
unsigned long bt_v3_footprint(BtV3Mem *m){
    unsigned long pcap = g_pool_cap ? g_pool_cap : POOL_CAP0;
    unsigned long psl  = g_bt_nslots ? g_bt_nslots : BT_NSLOTS0;
    unsigned long ccap = g_ctx_cap ? g_ctx_cap : CTX_CAP0;
    unsigned long csl  = g_ctx_nslots ? g_ctx_nslots : CTX_NSLOTS0;
    BtV3Mem t;
    t.bt_pool   = (unsigned long)sizeof(BtEntry)*pcap;
    t.bt_slot   = (unsigned long)sizeof(int)*psl;
    t.ctx_pool  = (unsigned long)sizeof(CtxEntry)*ccap;
    t.ctx_slot  = (unsigned long)sizeof(int)*csl;
    t.bloom     = BLOOM_BITS/8;
    t.luts      = (unsigned long)sizeof(int32_t)*(PRES+EXP2_FN);
    t.total = t.bt_pool+t.bt_slot+t.ctx_pool+t.ctx_slot+t.bloom+t.luts;
    t.bt_entry_sz = sizeof(BtEntry);
    t.ctx_entry_sz = sizeof(CtxEntry);
    t.bt_pool_n = pcap; t.ctx_pool_n = ccap; t.bloom_bits = BLOOM_BITS;
    t.lut_n = PRES;
#ifdef BCB_MCU
    t.is_mcu = 1;
#else
    t.is_mcu = 0;
#endif
    if(m) *m = t;
    return t.total;
}

static void v3_dist(unsigned int *cum, unsigned int scale, void *u){ (void)u; bt_v3_distribution(cum,scale); }
static void v3_train(unsigned char b, void *u){ (void)u; bt_v3_train(b); }
CecBT btv3_cec_bt(void){
    bt_v3_init();
    CecBT bt; bt.distribution=v3_dist; bt.train=v3_train; bt.user=NULL;
    return bt;
}

/* ── v5 mmap prior: 직렬화/복원 후크 ───────────────────────── */
void bt_v3_export(BtV3Snapshot *s){
    if(!s) return;
    s->pool       = g_pool;       s->pool_used  = g_pool_used;
    s->ctx_pool   = g_ctx_pool;   s->ctx_used   = g_ctx_used;
    s->ctx_slot   = g_ctx_slot;   s->ctx_nslots = g_ctx_nslots;
    s->bloom      = g_bloom;      s->bloom_bytes= BLOOM_BITS/8;
    s->window     = g_window;     s->win_len    = g_win_len;
    s->bt_entry_sz=(unsigned)sizeof(BtEntry); s->ctx_entry_sz=(unsigned)sizeof(CtxEntry);
    s->bt_max_depth=BT_MAX_DEPTH; s->bloom_bits=BLOOM_BITS; s->pres=PRES;
}

/* 신뢰 불가 prior 검증. caller(prior_parse)가 각 영역의 버퍼 범위는 이미 확인했다.
 * 여기서는 reader 가 실제로 dereference 하는 인덱스/체인/탐색 종료성을 확인한다. */
int bt_v3_validate(const BtV3Snapshot *s){
    if(!s) return -1;
    /* 빌드 서명: reader 는 컴파일된 stride(sizeof) 로 pool/ctx_pool 을 읽으므로 일치 필수. */
    if(s->bt_entry_sz!=sizeof(BtEntry) || s->ctx_entry_sz!=sizeof(CtxEntry) ||
       s->bt_max_depth!=BT_MAX_DEPTH || s->bloom_bits!=BLOOM_BITS ||
       s->pres!=PRES || s->bloom_bytes!=BLOOM_BITS/8)
        return -1;
    unsigned long nslot = s->ctx_nslots, cu = s->ctx_used, pu = s->pool_used;
    /* ctx_slot 은 mask=nslots-1 로 인덱싱(power-of-two 가정). 0 이면 ctx_slot[0] 이 OOB. */
    if(nslot==0 || (nslot & (nslot-1))!=0) return -1;
    const int *slot = (const int*)s->ctx_slot;
    const CtxEntry *cp = (const CtxEntry*)s->ctx_pool;
    const BtEntry  *pp = (const BtEntry*)s->pool;
    if(!slot || (!cp && cu) || (!pp && pu)) return -1;
    /* 슬롯 인덱스 범위 + 빈 슬롯 존재(선형탐사 종료 보장). 음수는 빈 슬롯으로 취급. */
    int has_empty = 0;
    for(unsigned long i=0;i<nslot;i++){
        int v = slot[i];
        if(v < 0){ has_empty = 1; continue; }
        if((unsigned long)v >= cu) return -1;
    }
    if(!has_empty) return -1;                       /* 빈 슬롯 없음 → lookup 무한 루프 */
    /* CtxEntry: ctx_len 범위 + first_entry 범위. */
    for(unsigned long i=0;i<cu;i++){
        if(cp[i].ctx_len > BT_MAX_DEPTH) return -1;
        int fe = cp[i].first_entry;
        if(fe >= 0 && (unsigned long)fe >= pu) return -1;
    }
    /* BtEntry: ctx_len 범위 + ctx_next 범위. */
    for(unsigned long i=0;i<pu;i++){
        if(pp[i].ctx_len > BT_MAX_DEPTH) return -1;
        int nx = pp[i].ctx_next;
        if(nx >= 0 && (unsigned long)nx >= pu) return -1;
    }
    /* next 체인 사이클/공유 검출: 유효 prior 는 각 pool 항목이 정확히 한 체인에 속한다.
     * visited 가 두 번 찍히면 사이클(무한 루프) 또는 공유 → 거부. */
    if(pu){
        unsigned char *seen = (unsigned char*)calloc(pu,1);
        if(!seen) return -1;                        /* 검증 불가 → 보수적 거부 */
        for(unsigned long i=0;i<cu;i++){
            int idx = cp[i].first_entry;
            while(idx >= 0){
                if((unsigned long)idx >= pu || seen[idx]){ free(seen); return -1; }
                seen[idx] = 1;
                idx = pp[idx].ctx_next;
            }
        }
        free(seen);
    }
    return 0;
}

int bt_v3_attach(const BtV3Snapshot *s){
    if(!s) return -1;
    if(s->bt_entry_sz!=sizeof(BtEntry) || s->ctx_entry_sz!=sizeof(CtxEntry) ||
       s->bt_max_depth!=BT_MAX_DEPTH || s->bloom_bits!=BLOOM_BITS ||
       s->pres!=PRES || s->bloom_bytes!=BLOOM_BITS/8)
        return -1;                                  /* 빌드 설정 불일치 */
    /* 외부(읽기전용) 버퍼로 globals 를 가리킨다. g_bt_slot 은 쓰기 전용 → 미사용. */
    g_pool      = (BtEntry*)s->pool;     g_pool_used  = s->pool_used;
    g_ctx_pool  = (CtxEntry*)s->ctx_pool;g_ctx_used   = s->ctx_used;
    g_ctx_slot  = (int*)s->ctx_slot;     g_ctx_nslots = s->ctx_nslots; g_ctx_mask = g_ctx_nslots-1;
    g_bloom     = (unsigned char*)s->bloom;
    g_bt_slot   = NULL;
    g_pool_cap  = g_pool_used; g_ctx_cap = g_ctx_used; g_bt_nslots = 0; g_bt_mask = 0;
    build_luts();                                   /* LUT 는 RAM 에 재생성(결정적) */
    int wl = s->win_len; if(wl<0) wl=0; if(wl>BT_MAX_DEPTH) wl=BT_MAX_DEPTH;
    memcpy(g_window, s->window, (size_t)wl);
    g_win_len = wl;
    g_attached = 1; g_frozen = 1;
    return 0;
}

void bt_v3_detach(void){
    g_pool=NULL; g_ctx_pool=NULL; g_ctx_slot=NULL; g_bloom=NULL; g_bt_slot=NULL;
    g_pool_used=g_ctx_used=0;
    free(CONF_LOG2); CONF_LOG2=NULL;
    free(EXP2_FRAC); EXP2_FRAC=NULL;
    g_attached=0; g_frozen=0;
}

CecBT btv3_cec_bt_from_prior(void){
    CecBT bt; bt.distribution=v3_dist; bt.train=v3_train; bt.user=NULL;
    return bt;                                      /* attach 는 호출자가 먼저 수행 */
}
