#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "bt_model.h"

#define BT_BUCKETS    (1<<18)
#define BT_POOL       (8*1024*1024)
#define BT_MAX_DEPTH  24
#define BT_LEVELS_V4  6
#define BLOOM_BITS    (1<<22)
#define BLOOM_MASK    (BLOOM_BITS-1)

typedef struct { unsigned char ctx[BT_MAX_DEPTH]; unsigned char ctx_len, next_byte; unsigned int freq; int chain; } BtEntry;
typedef struct { int buckets[BT_BUCKETS]; BtEntry pool[BT_POOL]; unsigned long pool_used; } BtTable;
typedef struct { unsigned char ctx[BT_MAX_DEPTH]; unsigned char ctx_len; unsigned int total_freq, unique_next; int chain; } CtxEntry;

#define CTX_BUCKETS (1<<19)
#define CTX_POOL    (4*1024*1024)
static CtxEntry  g_ctx_pool[CTX_POOL];
static int       g_ctx_buckets[CTX_BUCKETS];
static unsigned long g_ctx_used;
static BtTable *g_table;
static unsigned char g_bloom[BLOOM_BITS/8];
static unsigned char g_window[BT_MAX_DEPTH];
static int g_win_len;

typedef struct { int dmin,dmax; double weight; } BtLevel;
static BtLevel LEVELS[BT_LEVELS_V4] = {
    {1,2,0.9},{3,4,1.3},{5,8,1.5},{9,12,1.7},{13,16,1.5},{17,24,1.1}
};

static inline unsigned int fnv(const unsigned char *d, int n){unsigned int h=2166136261u;for(int i=0;i<n;i++){h^=d[i];h*=16777619u;}return h;}
static inline CtxEntry* ctx_find_h(const unsigned char *ctx, int cl, unsigned int ch){
    int idx=g_ctx_buckets[ch&(CTX_BUCKETS-1)];
    while(idx>=0){CtxEntry *e=&g_ctx_pool[idx];if(e->ctx_len==cl&&memcmp(e->ctx,ctx,cl)==0)return e;idx=e->chain;}
    return NULL;
}
static inline CtxEntry* ctx_get_h(const unsigned char *ctx, int cl, unsigned int ch){
    CtxEntry *e=ctx_find_h(ctx,cl,ch); if(e)return e;
    if(g_ctx_used>=CTX_POOL)return NULL;
    e=&g_ctx_pool[g_ctx_used];memcpy(e->ctx,ctx,cl);e->ctx_len=cl;e->total_freq=0;e->unique_next=0;
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
    int idx=g_table->buckets[nh];int is_new=1;
    while(idx>=0){
        BtEntry *e=&g_table->pool[idx];
        if(e->ctx_len==cl&&e->next_byte==next&&memcmp(e->ctx,ctx,cl)==0){e->freq++;is_new=0;break;}
        idx=e->chain;
    }
    if(is_new){
        if(g_table->pool_used>=BT_POOL)return;
        BtEntry *e=&g_table->pool[g_table->pool_used];
        memcpy(e->ctx,ctx,cl);e->ctx_len=cl;e->next_byte=next;e->freq=1;
        e->chain=g_table->buckets[nh]; g_table->buckets[nh]=g_table->pool_used++;
    }
    CtxEntry *cc=ctx_get_h(ctx,cl,ch);
    if(cc){cc->total_freq++;if(is_new)cc->unique_next++;}
}

void bt_v4_init(void){
    if(!g_table)g_table=(BtTable*)calloc(1,sizeof(BtTable));
    else memset(g_table,0,sizeof(BtTable));
    memset(g_table->buckets,-1,sizeof(g_table->buckets));
    memset(g_bloom,0,sizeof(g_bloom));
    memset(g_window,0,sizeof(g_window));
    memset(g_ctx_buckets,-1,sizeof(g_ctx_buckets));
    g_win_len=0; g_ctx_used=0; g_table->pool_used=0;
}
void bt_v4_free(void){free(g_table);g_table=NULL;}
void bt_v4_train(unsigned char b){
    for(int n=1;n<=g_win_len&&n<=BT_MAX_DEPTH;n++)
        bt_update(g_window+(g_win_len-n),n,b);
    if(g_win_len<BT_MAX_DEPTH) g_window[g_win_len++]=b;
    else{memmove(g_window,g_window+1,BT_MAX_DEPTH-1);g_window[BT_MAX_DEPTH-1]=b;}
}

/* 핵심: predict와 동일한 결과를 byte 256개 동시에 계산 */
static double predict_byte(unsigned char byte) {
    if (g_win_len == 0) return 1.0/256.0;
    double ws=0, wt=0;
    for (int lv=0; lv<BT_LEVELS_V4; lv++) {
        int dmin = LEVELS[lv].dmin, dmax = LEVELS[lv].dmax;
        int start = g_win_len<dmax?g_win_len:dmax;
        double lws=0, lwt=0;
        for (int n=start; n>=dmin; n--) {
            if (n > g_win_len) continue;
            const unsigned char *ctx = g_window + (g_win_len - n);
            if (!bloom_chk(ctx,n)) continue;
            unsigned int ch = fnv(ctx,n);
            CtxEntry *cc = ctx_find_h(ctx,n,ch);
            if (!cc || cc->total_freq == 0) continue;
            
            unsigned int nh = ch; nh ^= byte; nh *= 16777619u; nh &= (BT_BUCKETS-1);
            int idx = g_table->buckets[nh]; unsigned int af = 0;
            while (idx >= 0) {
                BtEntry *e = &g_table->pool[idx];
                if (e->ctx_len == n && e->next_byte == byte && memcmp(e->ctx, ctx, n) == 0) {
                    af = e->freq; break;
                }
                idx = e->chain;
            }
            double p = af > 0 ? (double)af/(double)cc->total_freq : 1.0/(double)(cc->total_freq+256);
            double conf = p * 256.0; if (conf < 1.0) conf = 1.0;
            double w = exp((double)n) * pow(conf, 20);
            lws += w*p; lwt += w;
        }
        if (lwt > 0) { ws += LEVELS[lv].weight * (lws/lwt); wt += LEVELS[lv].weight; }
    }
    if (wt <= 0) return 1.0/256.0;
    double prob = ws/wt;
    if (prob<1e-12) prob=1e-12;
    if (prob>1.0) prob=1.0;
    return prob;
}

void bt_v4_distribution(unsigned int *cum_out, unsigned int scale) {
    double probs[256];
    double s = 0;
    for (int b=0; b<256; b++) { probs[b] = predict_byte((unsigned char)b); s += probs[b]; }
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
unsigned long bt_v4_entries(void){return g_table?g_table->pool_used:0;}
