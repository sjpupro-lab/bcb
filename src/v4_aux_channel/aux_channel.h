/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* aux_channel.h — v4: 보조채널 (distribution blend 방식, 작업 항목 #4)
 * (파일명은 'aux' Windows 예약어 회피를 위해 aux_channel)
 *
 * v2(context mix) 가 bucket fragmentation 으로 폐기된 뒤 채택된 형태.
 * context 는 건드리지 않고, 별도 학습한 거시 통계 prior 를 BT 분포와 가중 blend 한다:
 *
 *   prior(b)   = P(type(b) | prev_type) × P(b | type(b))
 *   P_final(b) = α · P_BT(b) + (1−α) · prior(b)
 *
 * 인코더/디코더가 같은 코퍼스로 learn() 하고 같은 순서로 observe() 하므로 무손실.
 * 측정: byte_type 채널, α=0.985 에서 4권 중 3권 +0.4~0.6% (합계 +0.41%).
 */
#ifndef BCB_AUX_H
#define BCB_AUX_H

#include <stddef.h>
#include <stdint.h>
#include "ce_compress.h"

typedef struct AuxChannel AuxChannel;
struct AuxChannel {
    /* 코퍼스로 prior 통계 학습 (counts 리셋 후 재집계) */
    void (*learn)(AuxChannel *self, const uint8_t *corpus, size_t len);
    /* 현재 누적분포 cum[0..256] 을 prior 와 blend (in-place, 총합 scale 유지, 각 빈≥1) */
    void (*adjust)(AuxChannel *self, uint32_t *cum, uint32_t scale);
    /* 1바이트 관찰 — 내부 상태(prev_type 등) 갱신 */
    void (*observe)(AuxChannel *self, uint8_t b);
    /* 상태 초기화 (counts + prev_type) */
    void (*reset)(AuxChannel *self);
    uint32_t alpha_q16;   /* BT 가중치 α, Q16 정수 (65536 = 1.0 = aux 무시) */
    void *state;
};

/* 내장 채널 생성 */
AuxChannel *aux_byte_type_new(double alpha);       /* P(type|prev_type)·P(b|type) */
AuxChannel *aux_bigram_type_new(double alpha);     /* P(type|pt2,pt1)·P(b|type) */
AuxChannel *aux_case_pattern_new(double alpha);    /* P(case|prev_case)·P(b|case) */
AuxChannel *aux_whitespace_phase_new(double alpha);/* P(b|단어 내 위치 phase) */

/* 여러 채널을 순차 blend 로 결합 (adjust 가 체이닝됨). chs 인스턴스 소유권은 호출자.
 * 최대 8개. */
AuxChannel *aux_combo_new(AuxChannel **chs, int n);

void aux_free(AuxChannel *ch);          /* 단일 채널/combo 본체 해제 (combo 자식은 별도) */

/* bt_v3(정수 BT) + symdist(합=1) + 채널 blend 를 묶은 CecBT.
 * bt_v3_init() 과 ch->reset() 을 수행하므로 인코드/디코드 직전 동일 시작점을 보장한다.
 * user 포인터로 ch 를 전달한다. */
CecBT aux_cec_bt(AuxChannel *ch);

/* 학습 헬퍼: ch->learn(corpus) + bt_v4 를 corpus 로 학습.
 * aux_cec_bt() 직후 호출한다 (인코드/디코드 양쪽 동일하게). */
void aux_prime(AuxChannel *ch, const uint8_t *corpus, size_t len);

#endif /* BCB_AUX_H */
