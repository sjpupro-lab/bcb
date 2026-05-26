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

typedef struct BcbPrior BcbPrior;

/* 다음 bcb_prior_save 에 포함할 landmark 표 등록 (NULL/0 이면 landmark 없음).
 *   n   : context 길이 (bytes)
 *   k   : landmark 개수
 *   ctx : k*n 바이트 (context 들)
 *   cum : k*256 uint16 (각 landmark 의 byte width, 합 = CEC_RC_SCALE)
 * 인코더/디코더는 hit 시 이 정수 width 를 그대로 써서 비트 동일·무손실. */
void bcb_prior_set_landmarks(int n, unsigned k, const unsigned char *ctx, const uint16_t *cum);

/* 현재 학습된 btv3 globals 를 path 로 직렬화. 0 ok / -1 실패. */
int bcb_prior_save(const char *path);

/* prior 파일을 mmap 으로 로드 (RAM 에 거의 복사 안 함). 실패 시 NULL. */
BcbPrior *bcb_prior_mmap(const char *path);

/* mmap 해제 + 핸들 free. */
void bcb_prior_close(BcbPrior *p);

/* btv3 를 이 prior 에 attach (frozen 읽기전용). 0 ok / -1 빌드 서명 불일치. */
int bcb_prior_attach(BcbPrior *p);

/* attach 후 prior 로 인코드/디코드할 CecBT 반환. */
CecBT bcb_prior_cec_bt(BcbPrior *p);

#endif /* BCB_PRIOR_H */
