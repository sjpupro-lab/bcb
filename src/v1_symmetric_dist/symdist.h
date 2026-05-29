/* BCB — Binary Compression by BT
 * Copyright (c) 2026 호시 <jahyag@gmail.com>
 * Proprietary — All Rights Reserved. See LICENSE.
 */
/* symdist.h — v1: 대칭쌍 분포 활용 (작업 항목 #1)
 *
 * 호시 통찰:
 *   - 분포는 합이 1인 대칭쌍
 *   - 절반이 어딘지 자동으로 알 수 있고
 *   - 정해진 공간 재귀라서 순서 또한 알 수 있음
 *
 * 본 모듈은 단계 (a) "합=1 강제" 를 구현한다.
 * v0 의 bt_v4_distribution 출력을 받아 누적합이 정확히 scale 이 되도록
 * 재정규화한다 (단조성·각 빈 >=1 보장). 인코더/디코더가 동일 함수를 쓰므로
 * 무손실은 유지된다.
 *
 * 단계 (b)(c) (대칭쌍/계층 분할) 는 ablation 으로 측정 후 결정한다 — docs 참고.
 */
#ifndef BCB_SYMDIST_H
#define BCB_SYMDIST_H

#include <stdint.h>
#include "ce_compress.h"

/* (a) 합=1 강제: cum[0..256] 을 받아 cum[256]==scale 이 되도록 재정규화.
 * cum[0]==0, 단조 증가, 각 빈 폭 >=1 을 보장한다. in-place. */
void symdist_normalize(uint32_t *cum, uint32_t scale);

/* v0 bt_v4 를 감싸 distribution 후 symdist_normalize 를 적용하는 BT.
 * cec_default_bt() 와 동일하게 bt_v4_init() 으로 상태를 리셋한다. */
CecBT symdist_bt(void);

#endif /* BCB_SYMDIST_H */
