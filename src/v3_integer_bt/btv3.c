/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* btv3.c — v3 BT
 *
 * 단계 1: distribution caching (활성 context 1회 탐색 + per-context next-byte 순회).
 * 단계 2: open addressing + bloom 4M→16M.
 *   v0 의 chained hash (8M entries / 256K buckets → 평균 체인 32) 가 대규모 학습 시
 *   삽입마다 긴 체인을 훑어 O(n²) 에 가까워졌다. 슬롯 16M(BtEntry)/8M(CtxEntry) 의
 *   선형 탐사 open addressing 으로 load factor ≤0.5, 탐사 O(1) 화.
 *   같은 entry/freq 를 찾으므로 분포 출력은 v0 와 비트 동일.
 *
 * 후속: 정수 LUT(3), MCU(4).
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "btv3.h"

#define BT_POOL       (8*1024*1024)
#define BT_SLOTS      (1<<24)            /* 16M open-addr 슬롯 (load ≤ 0.5) */
#define BT_SLOT_MASK  (BT_SLOTS-1)
#define BT_MAX_DEPTH  24
#define BT_LEVELS_V4  6
#define BLOOM_BITS    (1<<24)            /* 16M bits (2MB) — 단계 2 확장 */
#define BLOOM_MASK    (BLOOM_BITS-1)

#define CTX_POOL      (4*1024*1024)
#define CTX_SLOTS     (1<<23)            /* 8M open-addr 슬롯 */
#define CTX_SLOT_MASK (CTX_SLOTS-1)

typedef struct { unsigned char ctx[BT_MAX_DEPTH]; unsigned char ctx_len, next_byte; unsigned int freq; int ctx_next; } BtEntry;
typedef struct { unsigned char ctx[BT_MAX_DEPTH]; unsigned char ctx_len; unsigned int total_freq, unique_next; int first_entry; } CtxEntry;

static CtxEntry  g_ctx_pool[CTX_POOL];
static int      *g_ctx_slot;             /* heap, CTX_SLOTS */
static unsigned long g_ctx_used;
static BtEntry  *g_pool;                 /* heap, BT_POOL */
static int      *g_bt_slot;              /* heap, BT_SLOTS */
static unsigned long g_pool_used;
static unsigned char g_bloom[BLOOM_BITS/8];
static unsigned char g_window[BT_MAX_DEPTH];
static int g_win_len;
static double EXP_N[BT_MAX_DEPTH + 1];

typedef struct { int dmin,dmax; double weight; } BtLevel;
static BtLevel LEVELS[BT_LEVELS_V4] = {
    {1,2,0.9},{3,4,1.3},{5,8,1.5},{9,12,1.7},{13,16,1.5},{17,24,1.1}
};

/* pow(x,20) = x^20 (정수 지수) → 5회 곱 */
static inline double pow20(double x){ double a=x*x; a=a*a; double b=a; a=a*a; a=a*a; return a*b; }

static inline unsigned int fnv(const unsigned char *d, int n){unsigned int h=2166136261u;for(int i=0;i<n;i++){h^=d[i];h*=16777619u;}return h;}

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

    /* BtEntry open-addr find / insert */
    unsigned int h = ((ch ^ next) * 16777619u) & BT_SLOT_MASK;
    int is_new; long new_idx = -1;
    for(;;){
        int s = g_bt_slot[h];
        if(s < 0){
            if(g_pool_used >= BT_POOL) return;   /* pool full: v0 와 동일하게 ctx 갱신 없이 반환 */
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

void bt_v3_init(void){
    if(!g_pool)     g_pool     = (BtEntry*)malloc(sizeof(BtEntry)*BT_POOL);
    if(!g_bt_slot)  g_bt_slot  = (int*)malloc(sizeof(int)*BT_SLOTS);
    if(!g_ctx_slot) g_ctx_slot = (int*)malloc(sizeof(int)*CTX_SLOTS);
    memset(g_bt_slot,  -1, sizeof(int)*BT_SLOTS);
    memset(g_ctx_slot, -1, sizeof(int)*CTX_SLOTS);
    memset(g_bloom, 0, sizeof(g_bloom));
    memset(g_window, 0, sizeof(g_window));
    g_win_len=0; g_ctx_used=0; g_pool_used=0;
    for(int n=0;n<=BT_MAX_DEPTH;n++) EXP_N[n]=exp((double)n);
}
void bt_v3_free(void){
    free(g_pool); g_pool=NULL;
    free(g_bt_slot); g_bt_slot=NULL;
    free(g_ctx_slot); g_ctx_slot=NULL;
}
void bt_v3_train(unsigned char b){
    for(int n=1;n<=g_win_len&&n<=BT_MAX_DEPTH;n++)
        bt_update(g_window+(g_win_len-n),n,b);
    if(g_win_len<BT_MAX_DEPTH) g_window[g_win_len++]=b;
    else{memmove(g_window,g_window+1,BT_MAX_DEPTH-1);g_window[BT_MAX_DEPTH-1]=b;}
}

/* base(미관측 기본) + delta(관측 보정) 로 256바이트 분포를 한 번에 계산 */
void bt_v3_distribution(unsigned int *cum_out, unsigned int scale) {
    double probs[256];

    if (g_win_len == 0) {
        for (int b = 0; b < 256; b++) probs[b] = 1.0/256.0;
    } else {
        double ws[256], wt[256], dlws[256], dlwt[256];
        for (int b = 0; b < 256; b++) { ws[b]=0; wt[b]=0; }

        for (int lv = 0; lv < BT_LEVELS_V4; lv++) {
            int dmin = LEVELS[lv].dmin, dmax = LEVELS[lv].dmax;
            int start = g_win_len<dmax?g_win_len:dmax;
            for (int b = 0; b < 256; b++) { dlws[b]=0; dlwt[b]=0; }
            double base_lws=0, base_lwt=0; int any=0;

            for (int n=start; n>=dmin; n--) {
                if (n > g_win_len) continue;
                const unsigned char *ctx = g_window + (g_win_len - n);
                if (!bloom_chk(ctx,n)) continue;
                unsigned int ch = fnv(ctx,n);
                CtxEntry *cc = ctx_find_h(ctx,n,ch);
                if (!cc || cc->total_freq == 0) continue;
                any = 1;
                double total = (double)cc->total_freq;
                double en = EXP_N[n];
                double p0 = 1.0/(total+256.0);
                double conf0 = p0*256.0; if (conf0<1.0) conf0=1.0;
                double w0 = en*pow20(conf0);
                base_lws += w0*p0; base_lwt += w0;
                for (int idx=cc->first_entry; idx>=0; idx=g_pool[idx].ctx_next) {
                    BtEntry *e = &g_pool[idx];
                    int b = e->next_byte;
                    double p = (double)e->freq/total;
                    double conf = p*256.0; if (conf<1.0) conf=1.0;
                    double w = en*pow20(conf);
                    dlws[b] += w*p - w0*p0;
                    dlwt[b] += w - w0;
                }
            }
            if (any) {
                double Wlv = LEVELS[lv].weight;
                for (int b = 0; b < 256; b++) {
                    double lwt_b = base_lwt + dlwt[b];
                    if (lwt_b > 0) {
                        double lws_b = base_lws + dlws[b];
                        ws[b] += Wlv*(lws_b/lwt_b);
                        wt[b] += Wlv;
                    }
                }
            }
        }
        for (int b = 0; b < 256; b++) {
            double prob = wt[b]>0 ? ws[b]/wt[b] : 1.0/256.0;
            if (prob<1e-12) prob=1e-12;
            if (prob>1.0) prob=1.0;
            probs[b]=prob;
        }
    }

    double s = 0;
    for (int b=0; b<256; b++) s += probs[b];
    if (s <= 0) { unsigned int w=scale/256; for(int b=0;b<256;b++)cum_out[b]=b*w; cum_out[256]=scale; return; }
    double alpha=0.05, u=1.0/256.0;
    unsigned int acc = 0;
    for (int b=0; b<256; b++) {
        double m = (1-alpha)*(probs[b]/s) + alpha*u;
        unsigned int w = (unsigned int)(m*scale);
        if (w<1) w=1;
        cum_out[b] = acc; acc += w;
    }
    cum_out[256] = acc;
}
unsigned long bt_v3_entries(void){return g_pool_used;}

static void v3_dist(unsigned int *cum, unsigned int scale, void *u){ (void)u; bt_v3_distribution(cum,scale); }
static void v3_train(unsigned char b, void *u){ (void)u; bt_v3_train(b); }
CecBT btv3_cec_bt(void){
    bt_v3_init();
    CecBT bt; bt.distribution=v3_dist; bt.train=v3_train; bt.user=NULL;
    return bt;
}
