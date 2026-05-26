/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* symdist.c — v1 단계 (a) 구현 */
#include "symdist.h"
#include "bt_model.h"

void symdist_normalize(uint32_t *cum, uint32_t scale) {
    /* 256개 빈 폭을 추출 */
    uint32_t w[256];
    uint32_t total = cum[256];
    if (total == 0) {
        uint32_t base = scale / 256;
        for (int b = 0; b < 256; b++) w[b] = base ? base : 1;
    } else {
        for (int b = 0; b < 256; b++) w[b] = cum[b + 1] - cum[b];
    }

    /* 각 빈을 scale 비례로 재양자화, 최소 1 보장 */
    uint64_t sum = 0;
    for (int b = 0; b < 256; b++) {
        uint64_t nw = total ? ((uint64_t)w[b] * scale) / total : w[b];
        if (nw < 1) nw = 1;
        w[b] = (uint32_t)nw;
        sum += nw;
    }

    /* 합이 정확히 scale 이 되도록 보정 (가장 큰 빈에서 가감) */
    if (sum != scale) {
        int big = 0;
        for (int b = 1; b < 256; b++) if (w[b] > w[big]) big = b;
        if (sum < scale) {
            w[big] += (uint32_t)(scale - sum);
        } else {
            uint32_t excess = (uint32_t)(sum - scale);
            /* 큰 빈부터 줄이되 각 빈 >=1 유지 */
            while (excess > 0) {
                int idx = 0;
                for (int b = 1; b < 256; b++) if (w[b] > w[idx]) idx = b;
                uint32_t take = w[idx] - 1;
                if (take > excess) take = excess;
                if (take == 0) break;  /* 모두 1 이면 더 줄일 수 없음 */
                w[idx] -= take;
                excess -= take;
            }
            if (excess > 0) {
                /* 극단적 경우: scale < 256 이면 보정 불가. 그대로 둠 (호출자 책임) */
            }
        }
    }

    /* cum 재구성 */
    uint32_t acc = 0;
    for (int b = 0; b < 256; b++) { cum[b] = acc; acc += w[b]; }
    cum[256] = acc;
}

static void symdist_dist(uint32_t *cum, uint32_t scale, void *u) {
    (void)u;
    bt_v4_distribution(cum, scale);
    symdist_normalize(cum, scale);
}
static void symdist_train(uint8_t b, void *u) {
    (void)u;
    bt_v4_train(b);
}

CecBT symdist_bt(void) {
    bt_v4_init();
    CecBT bt;
    bt.distribution = symdist_dist;
    bt.train        = symdist_train;
    bt.user         = NULL;
    return bt;
}
