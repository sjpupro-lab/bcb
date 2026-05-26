/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* btv3.h — v3: 최적화 BT (작업 항목 #3)
 *
 * 단계 1 (이 파일): distribution caching.
 *   v0 bt_v4 는 위치마다 predict_byte 를 256회 호출, 매 호출이 context 탐색
 *   (bloom/fnv/hash) + exp/pow 를 중복 수행했다. v3 는 활성 context 를 한 번만
 *   찾고, 각 context 의 관측 next-byte 를 직접 순회(per-context 링크)하여 분포를
 *   한 번에 계산한다. 결과는 v0 와 수학적으로 동일(부동소수 재결합·pow→정수승만 차이).
 *
 * 후속 단계: open addressing + bloom 확장(2), 정수 LUT(3), MCU(4).
 * API 는 v0 와 동일 형태(bt_v3_*)로, 동일 링크 시 v0 와 head-to-head 비교 가능.
 */
#ifndef BCB_BTV3_H
#define BCB_BTV3_H

#include "ce_compress.h"

void bt_v3_init(void);
void bt_v3_free(void);
void bt_v3_train(unsigned char b);
void bt_v3_distribution(unsigned int *cum, unsigned int scale);
unsigned long bt_v3_entries(void);

/* 메모리 footprint 분해 (컴파일된 설정 기준; -DBCB_MCU 여부 포함) */
typedef struct {
    unsigned long bt_pool, bt_slot, ctx_pool, ctx_slot, bloom, luts, total;
    unsigned long bt_entry_sz, ctx_entry_sz, bt_pool_n, ctx_pool_n, bloom_bits, lut_n;
    int is_mcu;
} BtV3Mem;
unsigned long bt_v3_footprint(BtV3Mem *m);   /* total bytes 반환, m 채움(NULL 가능) */

/* bt_v3 를 쓰는 CecBT (bt_v3_init() 수행) */
CecBT btv3_cec_bt(void);

#endif /* BCB_BTV3_H */
