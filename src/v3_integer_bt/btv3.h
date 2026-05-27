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

/* ── 스레드 안전 reader (de-globalized read path) ──────────
 * 읽기 전용 prior 데이터(pool/ctx/bloom, mmap 공유)를 가리키고, 가변 상태(window)는
 * 인스턴스마다 따로 가진다. distribution_r 은 전역을 안 쓰므로(LUT 만 read-only 공유)
 * 서로 다른 reader 를 여러 스레드가 동시에 써도 안전하다. */
#define BCB_BT_MAX_DEPTH 24
typedef struct {
    const void *pool;        /* BtEntry[]  (read-only) */
    const void *ctx_pool;    /* CtxEntry[] (read-only) */
    const int  *ctx_slot;    /* (read-only) */
    const unsigned char *bloom;
    unsigned long ctx_mask;
    unsigned char window[BCB_BT_MAX_DEPTH];   /* 가변, per-instance */
    int win_len;
} BtV3Reader;

void bt_v3_ensure_luts(void);                 /* LUT 1회 생성 (스레드 생성 전 호출) */
void bt_v3_reader_reset_window(BtV3Reader *r);
void bt_v3_reader_push(BtV3Reader *r, unsigned char b);   /* window 전진 (frozen) */
void bt_v3_distribution_r(const BtV3Reader *r, unsigned int *cum, unsigned int scale);
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

/* ── v5 mmap prior 지원 ────────────────────────────────────
 * 학습된 BT 를 직렬화/복원하기 위한 후크. 분포 계산은 g_ctx_pool/g_ctx_slot/
 * g_pool/g_bloom + g_window 만 읽으므로(쓰기 전용 g_bt_slot 불필요), 이들을
 * 외부(파일 mmap) 버퍼로 가리켜 읽기 전용으로 동작시킨다. "frozen" 모드에선
 * train 이 window 만 전진시키고 pool 을 수정하지 않는다. */
typedef struct {
    const void *pool;      unsigned long pool_used;     /* BtEntry[]  */
    const void *ctx_pool;  unsigned long ctx_used;      /* CtxEntry[] */
    const void *ctx_slot;  unsigned long ctx_nslots;    /* int[]      */
    const void *bloom;     unsigned long bloom_bytes;   /* bit array  */
    const unsigned char *window; int win_len;
    /* 빌드 호환성 서명 (불일치 시 attach 거부) */
    unsigned bt_entry_sz, ctx_entry_sz, bt_max_depth, bloom_bits, pres;
} BtV3Snapshot;

void bt_v3_freeze(int on);              /* train 시 pool 갱신 끄기/켜기 */
void bt_v3_reset_window(void);          /* context window 비우기 (메시지 독립 인코딩) */
void bt_v3_set_window(const unsigned char *ctx, int len);  /* window 를 주어진 context 로 설정 */
int  bt_v3_window(const unsigned char **out);  /* 현재 window: 반환 길이, *out=window 시작 */

/* ── landmark 추출용 context 열람 (학습된 상태에서) ──────── */
unsigned long bt_v3_ctx_pool_count(void);
/* i 번째 CtxEntry: ctx_out(>=BT_MAX_DEPTH), *len, *total, freq256(256개, 미관측 0) 채움.
 * 반환 0 ok / -1 범위초과. */
int bt_v3_ctx_at(unsigned long i, unsigned char *ctx_out, int *len_out,
                 unsigned *total_out, unsigned *freq256);
void bt_v3_export(BtV3Snapshot *s);     /* 학습 후: 현재 globals 포인터/크기 채움 */
int  bt_v3_attach(const BtV3Snapshot *s); /* globals 를 외부 읽기전용 버퍼로; freeze; 0 ok / -1 서명 불일치 */
void bt_v3_detach(void);                /* attach 해제 (외부 버퍼 free 안 함) */

/* attach 된 prior 로 동작하는 CecBT (bt_v3_init 호출 안 함) */
CecBT btv3_cec_bt_from_prior(void);

#endif /* BCB_BTV3_H */
