/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* btv3.c — v3 단계 1: distribution caching
 *
 * v0(bt_model.c) 에서 바뀐 점:
 *   - CtxEntry.first_entry / BtEntry.ctx_next: context 별 관측 next-byte 링크드 리스트.
 *   - bt_v3_distribution: 활성 context 1회 탐색 후, base(미관측 기본기여) + delta(관측 보정)
 *     로 256바이트 분포를 한 번에 계산. predict_byte 256회 호출 제거.
 *   - exp(n) LUT (n 정수) + pow(x,20) → 정수승 pow20() (libm pow 제거, 5회 곱).
 * 자료구조·정규화는 v0 와 동일. 결과는 v0 와 ±부동소수 오차 내 동일.
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "btv3.h"

#define BT_BUCKETS    (1<<18)
#define BT_POOL       (8*1024*1024)
#define BT_MAX_DEPTH  24
#define BT_LEVELS_V4  6
#define BLOOM_BITS    (1<<22)
#define BLOOM_MASK    (BLOOM_BITS-1)

typedef struct { unsigned char ctx[BT_MAX_DEPTH]; unsigned char ctx_len, next_byte; unsigned int freq; int chain; int ctx_next; } BtEntry;
typedef struct { int buckets[BT_BUCKETS]; BtEntry pool[BT_POOL]; unsigned long pool_used; } BtTable;
typedef struct { unsigned char ctx[BT_MAX_DEPTH]; unsigned char ctx_len; unsigned int total_freq, unique_next; int chain; int first_entry; } CtxEntry;

#define CTX_BUCKETS (1<<19)
#define CTX_POOL    (4*1024*1024)
static CtxEntry  g_ctx_pool[CTX_POOL];
static int       g_ctx_buckets[CTX_BUCKETS];
static unsigned long g_ctx_used;
static BtTable *g_table;
static unsigned char g_bloom[BLOOM_BITS/8];
static unsigned char g_window[BT_MAX_DEPTH];
static int g_win_len;
static double EXP_N[BT_MAX_DEPTH + 1];

typedef struct { int dmin,dmax; double weight; } BtLevel;
static BtLevel LEVELS[BT_LEVELS_V4] = {
    {1,2,0.9},{3,4,1.3},{5,8,1.5},{9,12,1.7},{13,16,1.5},{17,24,1.1}
};

/* pow(x,20) = x^20, 정수 지수 → 5회 곱 (libm pow 대체) */
static inline double pow20(double x){ double a=x*x; a=a*a; double b=a; a=a*a; a=a*a; return a*b; }
/* a=x^2 → a=x^4 → b=x^4 → a=x^8 → a=x^16 → return x^16 * x^4 = x^20 */

static inline unsigned int fnv(const unsigned char *d, int n){unsigned int h=2166136261u;for(int i=0;i<n;i++){h^=d[i];h*=16777619u;}return h;}
static inline CtxEntry* ctx_find_h(const unsigned char *ctx, int cl, unsigned int ch){
    int idx=g_ctx_buckets[ch&(CTX_BUCKETS-1)];
    while(idx>=0){CtxEntry *e=&g_ctx_pool[idx];if(e->ctx_len==cl&&memcmp(e->ctx,ctx,cl)==0)return e;idx=e->chain;}
    return NULL;
}
static inline CtxEntry* ctx_get_h(const unsigned char *ctx, int cl, unsigned int ch){
    CtxEntry *e=ctx_find_h(ctx,cl,ch); if(e)return e;
    if(g_ctx_used>=CTX_POOL)return NULL;
    e=&g_ctx_pool[g_ctx_used];memcpy(e->ctx,ctx,cl);e->ctx_len=cl;e->total_freq=0;e->unique_next=0;e->first_entry=-1;
    int b=ch&(CTX_BUCKETS-1); e->chain=g_ctx_buckets[b]; g_ctx_buckets[b]=g_ctx_used++;
    return e;
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
    unsigned int nh=ch;nh^=next;nh*=16777619u;nh&=(BT_BUCKETS-1);
    int idx=g_table->buckets[nh];int is_new=1; long new_idx=-1;
    while(idx>=0){
        BtEntry *e=&g_table->pool[idx];
        if(e->ctx_len==cl&&e->next_byte==next&&memcmp(e->ctx,ctx,cl)==0){e->freq++;is_new=0;break;}
        idx=e->chain;
    }
    if(is_new){
        if(g_table->pool_used>=BT_POOL)return;
        new_idx=(long)g_table->pool_used;
        BtEntry *e=&g_table->pool[new_idx];
        memcpy(e->ctx,ctx,cl);e->ctx_len=cl;e->next_byte=next;e->freq=1;e->ctx_next=-1;
        e->chain=g_table->buckets[nh]; g_table->buckets[nh]=g_table->pool_used++;
    }
    CtxEntry *cc=ctx_get_h(ctx,cl,ch);
    if(cc){
        cc->total_freq++;
        if(is_new){
            cc->unique_next++;
            if(new_idx>=0){ g_table->pool[new_idx].ctx_next=cc->first_entry; cc->first_entry=(int)new_idx; }
        }
    }
}

void bt_v3_init(void){
    if(!g_table)g_table=(BtTable*)calloc(1,sizeof(BtTable));
    else memset(g_table,0,sizeof(BtTable));
    memset(g_table->buckets,-1,sizeof(g_table->buckets));
    memset(g_bloom,0,sizeof(g_bloom));
    memset(g_window,0,sizeof(g_window));
    memset(g_ctx_buckets,-1,sizeof(g_ctx_buckets));
    g_win_len=0; g_ctx_used=0; g_table->pool_used=0;
    for(int n=0;n<=BT_MAX_DEPTH;n++) EXP_N[n]=exp((double)n);
}
void bt_v3_free(void){free(g_table);g_table=NULL;}
void bt_v3_train(unsigned char b){
    for(int n=1;n<=g_win_len&&n<=BT_MAX_DEPTH;n++)
        bt_update(g_window+(g_win_len-n),n,b);
    if(g_win_len<BT_MAX_DEPTH) g_window[g_win_len++]=b;
    else{memmove(g_window,g_window+1,BT_MAX_DEPTH-1);g_window[BT_MAX_DEPTH-1]=b;}
}

/* base(미관측 기본) + delta(관측 보정) 로 256바이트 분포를 한 번에 계산.
 * predict_byte(b) 를 256회 부른 것과 수학적으로 동일. */
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
                for (int idx=cc->first_entry; idx>=0; idx=g_table->pool[idx].ctx_next) {
                    BtEntry *e = &g_table->pool[idx];
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
unsigned long bt_v3_entries(void){return g_table?g_table->pool_used:0;}

static void v3_dist(unsigned int *cum, unsigned int scale, void *u){ (void)u; bt_v3_distribution(cum,scale); }
static void v3_train(unsigned char b, void *u){ (void)u; bt_v3_train(b); }
CecBT btv3_cec_bt(void){
    bt_v3_init();
    CecBT bt; bt.distribution=v3_dist; bt.train=v3_train; bt.user=NULL;
    return bt;
}
