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
