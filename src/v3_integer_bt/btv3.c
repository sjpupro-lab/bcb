/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
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

/* ── 크기 설정: 데스크톱 기본 vs MCU(-DBCB_MCU) ──────────────
 * MCU: pool 64K(~2MB) / bloom 256K bit / ctx 16K / LUT 4K — ESP32·RP2040 대응. */
#ifdef BCB_MCU
  #define BT_POOL     (64*1024)
  #define BT_SLOTS    (1<<17)         /* 128K (load ≤0.5) */
  #define BLOOM_BITS  (1<<18)         /* 256K bit */
  #define CTX_POOL    (16*1024)
  #define CTX_SLOTS   (1<<15)         /* 32K */
  #define PRES_BITS   12              /* CONF_LOG2 4096 entries */
  #define EXP2_FB     12              /* EXP2_FRAC 4096 entries */
#else
  #define BT_POOL     (8*1024*1024)
  #define BT_SLOTS    (1<<24)
  #define BLOOM_BITS  (1<<24)
  #define CTX_POOL    (4*1024*1024)
  #define CTX_SLOTS   (1<<23)
  #define PRES_BITS   16
  #define EXP2_FB     16
#endif
#define BT_SLOT_MASK  (BT_SLOTS-1)
#define BT_MAX_DEPTH  24
#define BT_LEVELS_V4  6
#define BLOOM_MASK    (BLOOM_BITS-1)
#define CTX_SLOT_MASK (CTX_SLOTS-1)

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

static CtxEntry  g_ctx_pool[CTX_POOL];
static int      *g_ctx_slot;
static unsigned long g_ctx_used;
static BtEntry  *g_pool;
static int      *g_bt_slot;
static unsigned long g_pool_used;
static unsigned char g_bloom[BLOOM_BITS/8];
static unsigned char g_window[BT_MAX_DEPTH];
static int g_win_len;

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

/* ── CtxEntry open addressing ─────────────────────────────── */
static inline CtxEntry* ctx_find_h(const unsigned char *ctx, int cl, unsigned int ch){
    unsigned int h = ch & CTX_SLOT_MASK;
    for(;;){
        int s = g_ctx_slot[h];
        if(s < 0) return NULL;
        CtxEntry *e = &g_ctx_pool[s];
        if(e->ctx_len==cl && memcmp(e->ctx,ctx,cl)==0) return e;
        h = (h+1) & CTX_SLOT_MASK;
    }
}
static inline CtxEntry* ctx_get_h(const unsigned char *ctx, int cl, unsigned int ch){
    unsigned int h = ch & CTX_SLOT_MASK;
    for(;;){
        int s = g_ctx_slot[h];
        if(s < 0){
            if(g_ctx_used >= CTX_POOL) return NULL;
            int idx = (int)g_ctx_used;
            CtxEntry *e = &g_ctx_pool[idx];
            memcpy(e->ctx,ctx,cl); e->ctx_len=cl; e->total_freq=0; e->unique_next=0; e->first_entry=-1;
            g_ctx_slot[h] = idx; g_ctx_used++;
            return e;
        }
        CtxEntry *e = &g_ctx_pool[s];
        if(e->ctx_len==cl && memcmp(e->ctx,ctx,cl)==0) return e;
        h = (h+1) & CTX_SLOT_MASK;
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
    unsigned int h = ((ch ^ next) * 16777619u) & BT_SLOT_MASK;
    int is_new; long new_idx = -1;
    for(;;){
        int s = g_bt_slot[h];
        if(s < 0){
            if(g_pool_used >= BT_POOL) return;
            new_idx = (long)g_pool_used;
            BtEntry *e = &g_pool[new_idx];
            memcpy(e->ctx,ctx,cl); e->ctx_len=cl; e->next_byte=next; e->freq=1; e->ctx_next=-1;
            g_bt_slot[h] = (int)new_idx; g_pool_used++;
            is_new = 1; break;
        }
        BtEntry *e = &g_pool[s];
        if(e->ctx_len==cl && e->next_byte==next && memcmp(e->ctx,ctx,cl)==0){ e->freq++; is_new=0; break; }
        h = (h+1) & BT_SLOT_MASK;
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
    if(!g_pool)     g_pool     = (BtEntry*)malloc(sizeof(BtEntry)*BT_POOL);
    if(!g_bt_slot)  g_bt_slot  = (int*)malloc(sizeof(int)*BT_SLOTS);
    if(!g_ctx_slot) g_ctx_slot = (int*)malloc(sizeof(int)*CTX_SLOTS);
    build_luts();
    memset(g_bt_slot,  -1, sizeof(int)*BT_SLOTS);
    memset(g_ctx_slot, -1, sizeof(int)*CTX_SLOTS);
    memset(g_bloom, 0, sizeof(g_bloom));
    memset(g_window, 0, sizeof(g_window));
    g_win_len=0; g_ctx_used=0; g_pool_used=0;
}
void bt_v3_free(void){
    free(g_pool); g_pool=NULL;
    free(g_bt_slot); g_bt_slot=NULL;
    free(g_ctx_slot); g_ctx_slot=NULL;
    free(CONF_LOG2); CONF_LOG2=NULL;
    free(EXP2_FRAC); EXP2_FRAC=NULL;
}
void bt_v3_train(unsigned char b){
    for(int n=1;n<=g_win_len&&n<=BT_MAX_DEPTH;n++)
        bt_update(g_window+(g_win_len-n),n,b);
    if(g_win_len<BT_MAX_DEPTH) g_window[g_win_len++]=b;
    else{memmove(g_window,g_window+1,BT_MAX_DEPTH-1);g_window[BT_MAX_DEPTH-1]=b;}
}

/* 정수 전용 분포 계산. 레벨마다:
 *   pass A — 활성 context 수집 + log2_w 최대값(max_log2) 탐색
 *   pass B — w=EXP2(log2_w−max) 로 base(미관측)+delta(관측) 누적 → byte별 prob_lv
 * 레벨 가중 결합 → 정규화·alpha-blend·scale 양자화. */
void bt_v3_distribution(unsigned int *cum_out, unsigned int scale) {
    static int64_t probs[256];   /* Q28 */

    if (g_win_len == 0) {
        for (int b=0;b<256;b++) probs[b] = PSCALE/256;
    } else {
        int64_t ws[256], wt[256];
        for (int b=0;b<256;b++){ ws[b]=0; wt[b]=0; }

        /* 활성 context 캐시 + context별 byte 빈도표 (레벨 내) */
        static int      act_n[BT_MAX_DEPTH];
        static unsigned act_total[BT_MAX_DEPTH];
        static int64_t  act_l0[BT_MAX_DEPTH], act_p0[BT_MAX_DEPTH];
        static unsigned act_freq[BT_MAX_DEPTH][256];

        for (int lv=0; lv<BT_LEVELS_V4; lv++) {
            int dmin=LEVELS[lv].dmin, dmax=LEVELS[lv].dmax;
            int start = g_win_len<dmax?g_win_len:dmax;
            int nact=0;

            /* 활성 context 수집 + 빈도표 구축 */
            for (int n=start; n>=dmin; n--) {
                if (n>g_win_len) continue;
                const unsigned char *ctx = g_window + (g_win_len-n);
                if (!bloom_chk(ctx,n)) continue;
                unsigned int ch = fnv(ctx,n);
                CtxEntry *cc = ctx_find_h(ctx,n,ch);
                if (!cc || cc->total_freq==0) continue;
                unsigned total = cc->total_freq;
                int a = nact++;
                act_n[a]=n; act_total[a]=total;
                int idx0 = (int)(PRES/(int64_t)(total+256)); if(idx0>=PRES)idx0=PRES-1;
                act_l0[a] = (int64_t)EXP_LOG2[n] + CONF_LOG2[idx0];
                act_p0[a] = PSCALE/(int64_t)(total+256);
                memset(act_freq[a], 0, 256*sizeof(unsigned));
                for (int idx=cc->first_entry; idx>=0; idx=g_pool[idx].ctx_next)
                    act_freq[a][g_pool[idx].next_byte] = g_pool[idx].freq;
            }
            if (nact==0) continue;

            /* byte별로 자기 context들 중 max 로 정규화(scale-invariant, underflow 방지) */
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
                    } else {
                        l = act_l0[a]; p = act_p0[a];
                    }
                    tl[a]=l; tp[a]=p; if(l>max_l) max_l=l;
                }
                int64_t num=0, den=0;
                for (int a=0; a<nact; a++) {
                    int64_t w = exp2_w(tl[a] - max_l);
                    num += w*tp[a]; den += w;
                }
                if (den>0) {
                    int64_t prob_lv = num / den;             /* Q PBITS */
                    ws[b] += (int64_t)W * prob_lv;
                    wt[b] += (int64_t)W;
                }
            }
        }
        for (int b=0;b<256;b++) {
            int64_t prob = wt[b]>0 ? ws[b]/wt[b] : (PSCALE/256);
            if (prob<1) prob=1;
            if (prob>PSCALE) prob=PSCALE;
            probs[b]=prob;
        }
    }

    /* 정규화 + alpha-blend(0.95/0.05) + scale 양자화 (정수) */
    int64_t s = 0;
    for (int b=0;b<256;b++) s += probs[b];
    if (s <= 0) { unsigned int w=scale/256; for(int b=0;b<256;b++)cum_out[b]=b*w; cum_out[256]=scale; return; }
    int64_t base_w = (50*(int64_t)scale)/(1000*256);   /* alpha·scale/256 */
    unsigned int acc = 0;
    for (int b=0;b<256;b++) {
        int64_t w = (950*(int64_t)scale*probs[b])/(1000*s) + base_w;
        if (w<1) w=1;
        cum_out[b] = acc; acc += (unsigned int)w;
    }
    cum_out[256] = acc;
}
unsigned long bt_v3_entries(void){return g_pool_used;}

unsigned long bt_v3_footprint(BtV3Mem *m){
    BtV3Mem t;
    t.bt_pool   = (unsigned long)sizeof(BtEntry)*BT_POOL;
    t.bt_slot   = (unsigned long)sizeof(int)*BT_SLOTS;
    t.ctx_pool  = (unsigned long)sizeof(CtxEntry)*CTX_POOL;
    t.ctx_slot  = (unsigned long)sizeof(int)*CTX_SLOTS;
    t.bloom     = BLOOM_BITS/8;
    t.luts      = (unsigned long)sizeof(int32_t)*(PRES+EXP2_FN);
    t.total = t.bt_pool+t.bt_slot+t.ctx_pool+t.ctx_slot+t.bloom+t.luts;
    t.bt_entry_sz = sizeof(BtEntry);
    t.ctx_entry_sz = sizeof(CtxEntry);
    t.bt_pool_n = BT_POOL; t.ctx_pool_n = CTX_POOL; t.bloom_bits = BLOOM_BITS;
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
