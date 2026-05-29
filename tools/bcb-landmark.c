/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* bcb-landmark.c — Landmark Prior Index 측정 (PR-1).
 *
 *   bcb-landmark --corpus <path> [--train-size N] [--test-size M] [--n 8] [--md] [--label NAME]
 *
 * 학습 corpus 에서 빈출 N-byte context 를 뽑아 top-K(8/32/128/512/2048/4096)로 sweep 하고,
 * test 구간에서 두 가지를 측정한다:
 *   1) hit rate  — context 가 landmark 집합에 드는 byte 위치 비율(coverage).
 *   2) 압축비 잠재력 — landmark hit 위치에서 "sharp 경험적 분포(empirical+backoff)" 의
 *      cross-entropy 가 현재 "BT blend" 보다 낮은가? (낮아야 ratio 개선 여지가 있다)
 *
 * 이 도구는 캐싱(=BT 분포 재사용, ratio 불변, 속도만)과 모델 변경(=sharp 분포, ratio 변동)을
 * 구분해 실측한다. 실제 코딩 바이트가 아니라 entropy 추정치다 (실측 바이트는 PR-2/3).
 */
#define _POSIX_C_SOURCE 200809L
#include "ce_compress.h"
#include "btv3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <limits.h>

#define KMAX 4096
#define BACKOFF_W 64.0    /* 경험적 분포 backoff pseudo-count mass */

static unsigned char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    unsigned char *b = (unsigned char *)malloc((size_t)sz);
    size_t n = fread(b, 1, (size_t)sz, f); fclose(f);
    *out_len = n; return b;
}

/* context 해시맵 (open addressing). key = N bytes (N<=16). */
typedef struct { unsigned char ctx[16]; unsigned freq; int rank; unsigned *nb; unsigned total; } LM;
static LM *g_tab; static int *g_slot; static unsigned long g_nslots, g_mask, g_used, g_cap;

static unsigned hashN(const unsigned char *c, int n){ unsigned h=2166136261u; for(int i=0;i<n;i++){h^=c[i];h*=16777619u;} return h; }

static LM *lm_get(const unsigned char *c, int n){
    unsigned h = hashN(c,n) & g_mask;
    for(;;){
        int s = g_slot[h];
        if(s<0){
            if(g_used>=g_cap) return NULL;          /* 미리 충분히 잡음 */
            int idx=(int)g_used++;
            memcpy(g_tab[idx].ctx,c,(size_t)n); g_tab[idx].freq=0; g_tab[idx].rank=INT_MAX;
            g_tab[idx].nb=NULL; g_tab[idx].total=0;
            g_slot[h]=idx; return &g_tab[idx];
        }
        if(memcmp(g_tab[s].ctx,c,(size_t)n)==0) return &g_tab[s];
        h=(h+1)&g_mask;
    }
}
static LM *lm_find(const unsigned char *c, int n){
    unsigned h = hashN(c,n) & g_mask;
    for(;;){ int s=g_slot[h]; if(s<0) return NULL; if(memcmp(g_tab[s].ctx,c,(size_t)n)==0) return &g_tab[s]; h=(h+1)&g_mask; }
}
static int cmp_freq(const void *a, const void *b){ unsigned fa=((const LM*)a)->freq, fb=((const LM*)b)->freq; return fa<fb?1:(fa>fb?-1:0); }

int main(int argc, char **argv){
    const char *corpus_path=NULL, *label=NULL; size_t train_size=50000, test_size=50000; int N=8, md=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--corpus")&&i+1<argc) corpus_path=argv[++i];
        else if(!strcmp(argv[i],"--train-size")&&i+1<argc) train_size=(size_t)strtoul(argv[++i],NULL,10);
        else if(!strcmp(argv[i],"--test-size")&&i+1<argc) test_size=(size_t)strtoul(argv[++i],NULL,10);
        else if(!strcmp(argv[i],"--n")&&i+1<argc) N=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--label")&&i+1<argc) label=argv[++i];
        else if(!strcmp(argv[i],"--md")) md=1;
    }
    if(!corpus_path||N<1||N>16){ fprintf(stderr,"usage: %s --corpus <p> [--train-size N] [--test-size M] [--n 1..16] [--md] [--label L]\n",argv[0]); return 2; }

    size_t clen=0; unsigned char *corpus=slurp(corpus_path,&clen);
    if(!corpus){ fprintf(stderr,"cannot read %s\n",corpus_path); return 1; }
    if(train_size>=clen){ fprintf(stderr,"corpus too small\n"); return 1; }
    const unsigned char *train=corpus; size_t tl=train_size;
    const unsigned char *test=corpus+tl; size_t test_len=clen-tl;
    if(test_size<test_len) test_len=test_size;

    /* 해시맵 용량: train 의 N-gram 위치 수 상한. load<=0.5. */
    unsigned long approx = tl>(size_t)N ? tl-(size_t)N : 1;
    g_cap = approx+16; g_nslots=1; while(g_nslots < g_cap*2) g_nslots<<=1; g_mask=g_nslots-1;
    g_tab=(LM*)malloc(sizeof(LM)*g_cap); g_slot=(int*)malloc(sizeof(int)*g_nslots);
    memset(g_slot,-1,sizeof(int)*g_nslots); g_used=0;

    /* pass1: context 빈도 */
    for(size_t i=(size_t)N;i<tl;i++){ LM*e=lm_get(train+i-N,N); if(e) e->freq++; }
    /* top-Kmax 선정 (freq desc). 정렬 후 rank 부여 + nb 할당. 정렬은 슬롯을 깨므로 슬롯 재구축. */
    qsort(g_tab,g_used,sizeof(LM),cmp_freq);
    memset(g_slot,-1,sizeof(int)*g_nslots);
    for(unsigned long i=0;i<g_used;i++){
        LM*e=&g_tab[i];
        if((int)i<KMAX){ e->rank=(int)i; e->nb=(unsigned*)calloc(256,sizeof(unsigned)); e->total=0; }
        else e->rank=INT_MAX;
        unsigned h=hashN(e->ctx,N)&g_mask; while(g_slot[h]>=0) h=(h+1)&g_mask; g_slot[h]=(int)i;
    }
    /* pass2: top-Kmax context 의 next-byte 경험적 빈도 */
    for(size_t i=(size_t)N;i<tl;i++){
        LM*e=lm_find(train+i-N,N);
        if(e && e->rank<KMAX){ e->nb[train[i]]++; e->total++; }
    }

    /* BT blend 질의용 학습 */
    CecBT bt=btv3_cec_bt(); for(size_t i=0;i<tl;i++) bt.train(train[i],bt.user);

    /* test 위치별: rank, btblend bits(bb), landmark bits(lb) 1회 계산 */
    size_t positions = test_len>(size_t)N ? test_len-(size_t)N : 0;
    int *rk=(int*)malloc(sizeof(int)*positions); double *bb=(double*)malloc(sizeof(double)*positions);
    double *lb=(double*)malloc(sizeof(double)*positions);
    uint32_t cum[257];
    for(size_t p=0;p<positions;p++){
        size_t i=(size_t)N+p; const unsigned char *ctx=test+i-N; unsigned char nxt=test[i];
        bt_v3_set_window(ctx,N); bt_v3_distribution(cum,CEC_RC_SCALE);
        double btp=(double)(cum[nxt+1]-cum[nxt])/(double)CEC_RC_SCALE; if(btp<=0) btp=1.0/CEC_RC_SCALE;
        double bbits=-log2(btp);
        LM*e=lm_find(ctx,N); int rank=(e&&e->rank<KMAX&&e->total>0)?e->rank:INT_MAX;
        double lbits=bbits;
        if(rank!=INT_MAX){
            double emp=(double)e->nb[nxt]; double tot=(double)e->total;
            double P=emp/(tot+BACKOFF_W) + BACKOFF_W*btp/(tot+BACKOFF_W);
            if(P<=0) P=1.0/CEC_RC_SCALE;
            lbits=-log2(P);
        }
        rk[p]=rank; bb[p]=bbits; lb[p]=lbits;
    }
    bt_v3_free();

    int Ks[]={8,32,128,512,2048,4096}; int nK=6;
    if(md){
        if(label) printf("### %s (N=%d)\n\n",label,N);
        printf("| K | hit%% | base bits/B | land bits/B | base ratio | land ratio | gain%% |\n");
        printf("|---|---|---|---|---|---|---|\n");
    } else {
        printf("%s  N=%d  train=%zuB test=%zuB positions=%zu\n", corpus_path, N, tl, test_len, positions);
        printf("%6s %7s %12s %12s %11s %11s %7s\n","K","hit%","base b/B","land b/B","base x","land x","gain%");
    }
    for(int k=0;k<nK;k++){
        int K=Ks[k]; size_t hits=0; double base=0, land=0;
        for(size_t p=0;p<positions;p++){ base+=bb[p]; if(rk[p]<K){ hits++; land+=lb[p]; } else land+=bb[p]; }
        double hr = positions? 100.0*hits/positions : 0;
        double bpb_base = positions? base/positions : 0, bpb_land = positions? land/positions : 0;
        double rx_base = bpb_base>0? 8.0/bpb_base : 0, rx_land = bpb_land>0? 8.0/bpb_land : 0;
        double gain = base>0? 100.0*(base-land)/base : 0;
        if(md) printf("| %d | %.1f | %.3f | %.3f | %.2f | %.2f | %+.2f |\n",K,hr,bpb_base,bpb_land,rx_base,rx_land,gain);
        else printf("%6d %7.1f %12.3f %12.3f %11.2f %11.2f %+7.2f\n",K,hr,bpb_base,bpb_land,rx_base,rx_land,gain);
    }
    if(md) printf("\n");

    for(unsigned long i=0;i<g_used && (int)i<KMAX;i++) free(g_tab[i].nb);
    free(g_tab); free(g_slot); free(rk); free(bb); free(lb); free(corpus);
    return 0;
}
