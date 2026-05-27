/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Licensed under the MIT License. See LICENSE.
 */
/* bcb_prior.h — v5: 학습된 BT prior 의 직렬화 + mmap 로드.
 *
 * 동기: BCB 는 학습 prior 를 매번 RAM 에 전부 올린다(작은 학습도 수십~95MB).
 * 작은 메시지 압축기에는 과한 메모리다. 진짜 사용 패턴은 "양쪽이 같은 prior
 * 파일을 공유 → 매 메시지 압축/복원" 이므로 prior 를 RAM 에 다 올릴 필요가 없다.
 * 파일을 mmap 하여 disk 에서 직접 읽고(읽기 전용·frozen), 만진 페이지만 RSS 에
 * 올린다. 분포 계산은 btv3 의 동일 함수를 그대로 쓰므로 in-memory 와 비트 동일.
 *
 * 파일 포맷 (.bcb-prior): header(고정) + window + pool + ctx_pool + ctx_slot + bloom.
 *   분포 계산에 쓰지 않는 g_bt_slot(최대 테이블)은 저장하지 않는다.
 */
#ifndef BCB_PRIOR_H
#define BCB_PRIOR_H

#include "ce_compress.h"
#include <stdint.h>

#ifndef BCB_PRIOR_TYPE_DEFINED
#define BCB_PRIOR_TYPE_DEFINED
typedef struct BcbPrior BcbPrior;
#endif

/* 다음 bcb_prior_save 에 포함할 landmark 표 등록 (NULL/0 이면 landmark 없음).
 *   n   : context 길이 (bytes)
 *   k   : landmark 개수
 *   ctx : k*n 바이트 (context 들)
 *   cum : k*256 uint16 (각 landmark 의 byte width, 합 = CEC_RC_SCALE)
 * 인코더/디코더는 hit 시 이 정수 width 를 그대로 써서 비트 동일·무손실. */
void bcb_prior_set_landmarks(int n, unsigned k, const unsigned char *ctx, const uint16_t *cum);

/* 현재 학습된 btv3 globals 를 path 로 직렬화. 0 ok / -1 실패. */
int bcb_prior_save(const char *path);

/* save + 현재 학습된 BT 에서 길이 n 빈출 context top-k 를 뽑아 landmark 로 박는다.
 * (window 는 내부에서 보존·복원). 0 ok / -1 실패. */
int bcb_prior_save_with_landmarks(const char *path, int n, unsigned k);

/* save + record schema(structural landmark): corpus 를 rec_size 로 정렬해 자리별 byte/delta
 * 분포를 모으고, held-out 으로 자리별 mode 를 정해 정수 양자화 cum 으로 박는다.
 * 인코더/디코더가 자리 p 에서 동일 cum 사용(델타는 prev_record[p] 순환 시프트) → 무손실.
 * 0 ok / -1 실패. */
int bcb_prior_save_with_schema(const char *path, const unsigned char *corpus,
                               size_t corpus_len, int rec_size);

/* prior 파일을 mmap 으로 로드 (RAM 에 거의 복사 안 함). 실패 시 NULL. */
BcbPrior *bcb_prior_mmap(const char *path);

/* 메모리 버퍼를 복사·소유해 prior 로드. 실패 시 NULL. */
BcbPrior *bcb_prior_from_buffer(const void *data, size_t len);

/* mmap 된 prior 의 매핑 길이 (footprint 보고용; from_buffer 면 0). */
size_t bcb_prior_map_len(const BcbPrior *p);

/* mmap 해제 + 핸들 free. */
void bcb_prior_close(BcbPrior *p);

/* btv3 를 이 prior 에 attach (frozen 읽기전용). 0 ok / -1 빌드 서명 불일치. */
int bcb_prior_attach(BcbPrior *p);

/* attach 후 prior 로 인코드/디코드할 CecBT 반환. (schema 있으면 structural, 아니면
 * landmark/BT). 메시지 시작마다 호출하면 structural 상태(position/prev)가 초기화된다. */
CecBT bcb_prior_cec_bt(BcbPrior *p);

/* structural 메시지 경계 리셋 (position=0, prev record 0). encode/decode 시작 전 호출.
 * bcb_prior_cec_bt 도 내부에서 호출하므로, 같은 bt 로 재인코딩/디코딩할 때만 별도 필요. */
void bcb_prior_msg_begin(void);

/* ── per-instance codec (thread-safe) ──────────────────────
 * 전역 상태 대신 인스턴스에 가변 상태(window/structural position)를 담는다. 읽기 전용
 * prior(mmap 공유) + read-only LUT 만 공유하므로, 서로 다른 BcbCodec 을 여러 스레드가
 * 동시에 써도 안전하다. (prior 는 스레드 생성 전에 open 할 것 — LUT 1회 생성.) */
typedef struct BcbCodec BcbCodec;
BcbCodec *bcb_codec_new(BcbPrior *p);     /* prior 의 읽기 전용 데이터에 바인딩 */
void      bcb_codec_free(BcbCodec *c);
void      bcb_codec_begin(BcbCodec *c);   /* 메시지 경계 리셋 */
CecBT     bcb_codec_cec_bt(BcbCodec *c);  /* 이 codec 에 묶인 CecBT (전역 미사용) */

/* prior 가 record schema 를 가지면 record_size(>0), 없으면 0. */
int bcb_prior_record_size(const BcbPrior *p);

#endif /* BCB_PRIOR_H */
