/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* aux.c — v4 byte_type 채널 구현 */
#include "aux.h"
#include "symdist.h"
#include "bt_model.h"
#include <stdlib.h>
#include <string.h>

/* ── byte type 분류 (영어 텍스트 거시 통계용) ───────────────── */
#define NTYPES 7
enum { T_SPACE = 0, T_NL, T_LOWER, T_UPPER, T_DIGIT, T_PUNCT, T_OTHER };

static int byte_type(uint8_t b) {
    if (b == ' ' || b == '\t') return T_SPACE;
    if (b == '\n' || b == '\r') return T_NL;
    if (b >= 'a' && b <= 'z')   return T_LOWER;
    if (b >= 'A' && b <= 'Z')   return T_UPPER;
    if (b >= '0' && b <= '9')   return T_DIGIT;
    if (b >= 33 && b <= 126)    return T_PUNCT;   /* 남은 출력가능 ASCII */
    return T_OTHER;                                /* 제어/고위 바이트 */
}

typedef struct {
    uint32_t trans[NTYPES][NTYPES];   /* prev_type → type 전이 카운트 */
    uint32_t trans_tot[NTYPES];
    uint32_t bytecnt[NTYPES][256];    /* type 별 byte 카운트 */
    uint32_t type_tot[NTYPES];
    int prev_type;
} ByteTypeState;

static void bt_reset(AuxChannel *self) {
    ByteTypeState *s = (ByteTypeState*)self->state;
    memset(s, 0, sizeof(*s));
    s->prev_type = T_SPACE;
}

static void bt_learn(AuxChannel *self, const uint8_t *corpus, size_t len) {
    ByteTypeState *s = (ByteTypeState*)self->state;
    bt_reset(self);
    int prev = -1;
    for (size_t i = 0; i < len; i++) {
        int t = byte_type(corpus[i]);
        s->bytecnt[t][corpus[i]]++;
        s->type_tot[t]++;
        if (prev >= 0) { s->trans[prev][t]++; s->trans_tot[prev]++; }
        prev = t;
    }
    s->prev_type = (len > 0) ? byte_type(corpus[len - 1]) : T_SPACE;
}

static void bt_observe(AuxChannel *self, uint8_t b) {
    ByteTypeState *s = (ByteTypeState*)self->state;
    s->prev_type = byte_type(b);
}

static void bt_adjust(AuxChannel *self, uint32_t *cum, uint32_t scale) {
    ByteTypeState *s = (ByteTypeState*)self->state;
    uint32_t total_bt = cum[256];
    if (total_bt == 0) return;

    int pt = s->prev_type;
    double inv_trans = s->trans_tot[pt] ? 1.0 / (double)s->trans_tot[pt] : 0.0;
    double alpha = self->alpha;

    uint64_t acc = 0;
    uint32_t newcum[257];
    for (int b = 0; b < 256; b++) {
        uint32_t wbt = cum[b + 1] - cum[b];
        double pbt = (double)wbt / (double)total_bt;

        int t = byte_type((uint8_t)b);
        double ptype = inv_trans ? (double)s->trans[pt][t] * inv_trans : (1.0 / NTYPES);
        double pbyte = s->type_tot[t] ? (double)s->bytecnt[t][b] / (double)s->type_tot[t] : 0.0;
        double prior = ptype * pbyte;

        double pf = alpha * pbt + (1.0 - alpha) * prior;
        uint32_t wf = (uint32_t)(pf * (double)scale);
        newcum[b] = (uint32_t)acc;
        acc += wf;
    }
    newcum[256] = (uint32_t)acc;

    memcpy(cum, newcum, sizeof(uint32_t) * 257);
    symdist_normalize(cum, scale);   /* 총합 scale, 각 빈≥1 보장 → 무손실 안전 */
}

AuxChannel *aux_byte_type_new(double alpha) {
    AuxChannel *ch = (AuxChannel*)calloc(1, sizeof(AuxChannel));
    if (!ch) return NULL;
    ch->state = calloc(1, sizeof(ByteTypeState));
    if (!ch->state) { free(ch); return NULL; }
    ch->learn = bt_learn;
    ch->adjust = bt_adjust;
    ch->observe = bt_observe;
    ch->reset = bt_reset;
    ch->alpha = alpha;
    bt_reset(ch);
    return ch;
}

void aux_free(AuxChannel *ch) {
    if (!ch) return;
    free(ch->state);
    free(ch);
}

/* ════════════════════════════════════════════════════════════
 * bigram_type 채널 — P(type | prev2, prev1) · P(b | type)
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t trans2[NTYPES][NTYPES][NTYPES];   /* [pt2][pt1][t] */
    uint32_t trans2_tot[NTYPES][NTYPES];
    uint32_t bytecnt[NTYPES][256];
    uint32_t type_tot[NTYPES];
    int pt1, pt2;
} BigramState;

static void bg_reset(AuxChannel *self) {
    BigramState *s = (BigramState*)self->state;
    memset(s, 0, sizeof(*s));
    s->pt1 = s->pt2 = T_SPACE;
}
static void bg_learn(AuxChannel *self, const uint8_t *corpus, size_t len) {
    BigramState *s = (BigramState*)self->state;
    bg_reset(self);
    int pt2 = -1, pt1 = -1;
    for (size_t i = 0; i < len; i++) {
        int t = byte_type(corpus[i]);
        s->bytecnt[t][corpus[i]]++; s->type_tot[t]++;
        if (pt2 >= 0 && pt1 >= 0) { s->trans2[pt2][pt1][t]++; s->trans2_tot[pt2][pt1]++; }
        pt2 = pt1; pt1 = t;
    }
    s->pt1 = (len >= 1) ? byte_type(corpus[len - 1]) : T_SPACE;
    s->pt2 = (len >= 2) ? byte_type(corpus[len - 2]) : T_SPACE;
}
static void bg_observe(AuxChannel *self, uint8_t b) {
    BigramState *s = (BigramState*)self->state;
    s->pt2 = s->pt1; s->pt1 = byte_type(b);
}
static void bg_adjust(AuxChannel *self, uint32_t *cum, uint32_t scale) {
    BigramState *s = (BigramState*)self->state;
    uint32_t total_bt = cum[256]; if (total_bt == 0) return;
    int pt2 = s->pt2, pt1 = s->pt1;
    double inv = s->trans2_tot[pt2][pt1] ? 1.0 / (double)s->trans2_tot[pt2][pt1] : 0.0;
    double alpha = self->alpha;
    uint64_t acc = 0; uint32_t nc[257];
    for (int b = 0; b < 256; b++) {
        double pbt = (double)(cum[b + 1] - cum[b]) / (double)total_bt;
        int t = byte_type((uint8_t)b);
        double ptype = inv ? (double)s->trans2[pt2][pt1][t] * inv : (1.0 / NTYPES);
        double pbyte = s->type_tot[t] ? (double)s->bytecnt[t][b] / (double)s->type_tot[t] : 0.0;
        double pf = alpha * pbt + (1.0 - alpha) * (ptype * pbyte);
        nc[b] = (uint32_t)acc; acc += (uint32_t)(pf * (double)scale);
    }
    nc[256] = (uint32_t)acc;
    memcpy(cum, nc, sizeof(uint32_t) * 257);
    symdist_normalize(cum, scale);
}

/* ════════════════════════════════════════════════════════════
 * case_pattern 채널 — P(case | prev_case) · P(b | case)
 * case: 0=lower, 1=upper, 2=non-letter
 * ════════════════════════════════════════════════════════════ */
#define NCASE 3
static int case_class(uint8_t b) {
    if (b >= 'a' && b <= 'z') return 0;
    if (b >= 'A' && b <= 'Z') return 1;
    return 2;
}
typedef struct {
    uint32_t trans[NCASE][NCASE];
    uint32_t trans_tot[NCASE];
    uint32_t bytecnt[NCASE][256];
    uint32_t cls_tot[NCASE];
    int prev;
} CaseState;

static void cs_reset(AuxChannel *self) {
    CaseState *s = (CaseState*)self->state;
    memset(s, 0, sizeof(*s));
    s->prev = 2;
}
static void cs_learn(AuxChannel *self, const uint8_t *corpus, size_t len) {
    CaseState *s = (CaseState*)self->state;
    cs_reset(self);
    int prev = -1;
    for (size_t i = 0; i < len; i++) {
        int c = case_class(corpus[i]);
        s->bytecnt[c][corpus[i]]++; s->cls_tot[c]++;
        if (prev >= 0) { s->trans[prev][c]++; s->trans_tot[prev]++; }
        prev = c;
    }
    s->prev = (len > 0) ? case_class(corpus[len - 1]) : 2;
}
static void cs_observe(AuxChannel *self, uint8_t b) {
    ((CaseState*)self->state)->prev = case_class(b);
}
static void cs_adjust(AuxChannel *self, uint32_t *cum, uint32_t scale) {
    CaseState *s = (CaseState*)self->state;
    uint32_t total_bt = cum[256]; if (total_bt == 0) return;
    int pc = s->prev;
    double inv = s->trans_tot[pc] ? 1.0 / (double)s->trans_tot[pc] : 0.0;
    double alpha = self->alpha;
    uint64_t acc = 0; uint32_t nc[257];
    for (int b = 0; b < 256; b++) {
        double pbt = (double)(cum[b + 1] - cum[b]) / (double)total_bt;
        int c = case_class((uint8_t)b);
        double pcls = inv ? (double)s->trans[pc][c] * inv : (1.0 / NCASE);
        double pbyte = s->cls_tot[c] ? (double)s->bytecnt[c][b] / (double)s->cls_tot[c] : 0.0;
        double pf = alpha * pbt + (1.0 - alpha) * (pcls * pbyte);
        nc[b] = (uint32_t)acc; acc += (uint32_t)(pf * (double)scale);
    }
    nc[256] = (uint32_t)acc;
    memcpy(cum, nc, sizeof(uint32_t) * 257);
    symdist_normalize(cum, scale);
}

/* ════════════════════════════════════════════════════════════
 * whitespace_phase 채널 — P(b | phase), phase = 직전 공백 이후 비공백 길이
 * ════════════════════════════════════════════════════════════ */
#define PMAX 12
static int is_ws(uint8_t b) { return b == ' ' || b == '\t' || b == '\n' || b == '\r'; }
typedef struct {
    uint32_t bytecnt[PMAX + 1][256];
    uint32_t phase_tot[PMAX + 1];
    int phase;
} WsState;

static void ws_reset(AuxChannel *self) {
    WsState *s = (WsState*)self->state;
    memset(s, 0, sizeof(*s));
    s->phase = 0;
}
static void ws_learn(AuxChannel *self, const uint8_t *corpus, size_t len) {
    WsState *s = (WsState*)self->state;
    ws_reset(self);
    int phase = 0;
    for (size_t i = 0; i < len; i++) {
        int p = phase < PMAX ? phase : PMAX;
        s->bytecnt[p][corpus[i]]++; s->phase_tot[p]++;
        if (is_ws(corpus[i])) phase = 0; else phase++;
    }
    s->phase = phase;
}
static void ws_observe(AuxChannel *self, uint8_t b) {
    WsState *s = (WsState*)self->state;
    if (is_ws(b)) s->phase = 0; else s->phase++;
}
static void ws_adjust(AuxChannel *self, uint32_t *cum, uint32_t scale) {
    WsState *s = (WsState*)self->state;
    uint32_t total_bt = cum[256]; if (total_bt == 0) return;
    int p = s->phase < PMAX ? s->phase : PMAX;
    double inv = s->phase_tot[p] ? 1.0 / (double)s->phase_tot[p] : 0.0;
    double alpha = self->alpha;
    uint64_t acc = 0; uint32_t nc[257];
    for (int b = 0; b < 256; b++) {
        double pbt = (double)(cum[b + 1] - cum[b]) / (double)total_bt;
        double prior = inv ? (double)s->bytecnt[p][b] * inv : (1.0 / 256.0);
        double pf = alpha * pbt + (1.0 - alpha) * prior;
        nc[b] = (uint32_t)acc; acc += (uint32_t)(pf * (double)scale);
    }
    nc[256] = (uint32_t)acc;
    memcpy(cum, nc, sizeof(uint32_t) * 257);
    symdist_normalize(cum, scale);
}

/* 공통 생성 헬퍼 */
static AuxChannel *mk(size_t state_sz, double alpha,
                     void (*learn)(AuxChannel*, const uint8_t*, size_t),
                     void (*adjust)(AuxChannel*, uint32_t*, uint32_t),
                     void (*observe)(AuxChannel*, uint8_t),
                     void (*reset)(AuxChannel*)) {
    AuxChannel *ch = (AuxChannel*)calloc(1, sizeof(AuxChannel));
    if (!ch) return NULL;
    ch->state = calloc(1, state_sz);
    if (!ch->state) { free(ch); return NULL; }
    ch->learn = learn; ch->adjust = adjust; ch->observe = observe; ch->reset = reset;
    ch->alpha = alpha;
    reset(ch);
    return ch;
}

AuxChannel *aux_bigram_type_new(double alpha) {
    return mk(sizeof(BigramState), alpha, bg_learn, bg_adjust, bg_observe, bg_reset);
}
AuxChannel *aux_case_pattern_new(double alpha) {
    return mk(sizeof(CaseState), alpha, cs_learn, cs_adjust, cs_observe, cs_reset);
}
AuxChannel *aux_whitespace_phase_new(double alpha) {
    return mk(sizeof(WsState), alpha, ws_learn, ws_adjust, ws_observe, ws_reset);
}

/* ════════════════════════════════════════════════════════════
 * combo — 여러 채널 순차 결합
 * ════════════════════════════════════════════════════════════ */
typedef struct { AuxChannel *chs[8]; int n; } ComboState;

static void cb_reset(AuxChannel *self) {
    ComboState *s = (ComboState*)self->state;
    for (int i = 0; i < s->n; i++) s->chs[i]->reset(s->chs[i]);
}
static void cb_learn(AuxChannel *self, const uint8_t *corpus, size_t len) {
    ComboState *s = (ComboState*)self->state;
    for (int i = 0; i < s->n; i++) s->chs[i]->learn(s->chs[i], corpus, len);
}
static void cb_observe(AuxChannel *self, uint8_t b) {
    ComboState *s = (ComboState*)self->state;
    for (int i = 0; i < s->n; i++) s->chs[i]->observe(s->chs[i], b);
}
static void cb_adjust(AuxChannel *self, uint32_t *cum, uint32_t scale) {
    ComboState *s = (ComboState*)self->state;
    for (int i = 0; i < s->n; i++) s->chs[i]->adjust(s->chs[i], cum, scale);
}

AuxChannel *aux_combo_new(AuxChannel **chs, int n) {
    if (n > 8) n = 8;
    AuxChannel *ch = (AuxChannel*)calloc(1, sizeof(AuxChannel));
    if (!ch) return NULL;
    ComboState *s = (ComboState*)calloc(1, sizeof(ComboState));
    if (!s) { free(ch); return NULL; }
    for (int i = 0; i < n; i++) s->chs[i] = chs[i];
    s->n = n;
    ch->state = s;
    ch->learn = cb_learn; ch->adjust = cb_adjust; ch->observe = cb_observe; ch->reset = cb_reset;
    ch->alpha = 1.0;
    return ch;
}

/* ── CecBT 통합 ─────────────────────────────────────────────── */
static void aux_dist(uint32_t *cum, uint32_t scale, void *user) {
    AuxChannel *ch = (AuxChannel*)user;
    bt_v4_distribution(cum, scale);
    symdist_normalize(cum, scale);     /* v1 (a) */
    ch->adjust(ch, cum, scale);        /* v4 blend */
}
static void aux_train(uint8_t b, void *user) {
    AuxChannel *ch = (AuxChannel*)user;
    bt_v4_train(b);
    ch->observe(ch, b);
}

CecBT aux_cec_bt(AuxChannel *ch) {
    bt_v4_init();
    ch->reset(ch);
    CecBT bt;
    bt.distribution = aux_dist;
    bt.train        = aux_train;
    bt.user         = ch;
    return bt;
}

void aux_prime(AuxChannel *ch, const uint8_t *corpus, size_t len) {
    ch->learn(ch, corpus, len);
    for (size_t i = 0; i < len; i++) bt_v4_train(corpus[i]);
}
