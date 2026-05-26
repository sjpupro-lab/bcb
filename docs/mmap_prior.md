# mmap prior (v5) — 직렬화된 prior 를 disk 에서 직접 읽기

`src/v5_mmap_prior/`. 학습된 BT prior 를 `.bcb-prior` 파일로 직렬화하고, 인코더·
디코더가 그 파일을 **mmap** 하여 공유한다. 재현: `make prior`.

## 동기와 결론 요약

원 설계 동기는 "in-memory 95MB → mmap <5MB" 였다. 실측 결과 이 목표는 **부분적으로만
성립**한다 — 아래를 정직하게 보고한다.

- **성립**: mmap 출력은 in-memory 와 **비트 단위 동일**(5개 시나리오 검증), 압축비 동일,
  round-trip 무손실.
- **성립**: prior 빌드와 사용이 분리되어 **시작 즉시 압축 가능**(학습 재실행 불필요).
  큰 prior 에서 효과가 크다 — 300KB 학습 prior 기준 **3.74s → 0.028s (130×)**.
- **성립**: 파일은 정확한 크기(예 14.5MB)로 매핑된다. in-memory 의 pow2 반올림
  capacity(94.5MB)와 달리 미사용 용량을 잡지 않는다. 읽기 전용 페이지는 page cache 로
  **프로세스 간 공유**된다(N개 인코더 ≈ 1× prior RAM).
- **불성립**: 단일 프로세스 **peak RSS 는 <5MB 로 떨어지지 않는다.** 해시 기반 조회
  (bloom·slot probing)가 prior 페이지 전반에 흩어져 접근하므로 메시지 1개만 압축해도
  대부분의 페이지가 resident 가 된다.
- **진짜 RAM 하한 레버는 mmap 이 아니라 MCU 빌드**(소형 고정 pool + 소형 bloom)다.

### "95MB" 의 정체 — capacity vs RSS

`make meminfo` 의 94.5MB 는 **할당 capacity**(g_pool_cap 을 pow2 로 반올림한 추정치)이며
*diverse 영어* 30KB 학습(617K context)이 기준이다. 실제 resident RSS 는 코퍼스 다양성에
크게 좌우된다:

| 학습 | 코퍼스 | BT entries | in-memory VmHWM |
|------|--------|-----------|------------------|
| 50 KB | http_headers (반복적) | 150,206 | **19.4 MB** |
| 300 KB | 영어 책 (diverse) | 5,147,808 | 495 MB |

BCB 가 노리는 **반복적 작은 메시지 도메인**에선 unique context 가 적어 prior 자체가 작다
(50KB http → 19MB). diverse 텍스트는 수백 MB까지 커진다. 어느 쪽이든 "95MB" 는 단일 고정
숫자가 아니다.

## 측정 — RSS · 처리량 (http_headers, train 50KB, 128B 메시지)

`make prior-rss` 실측:

| 모드 | prior 출처 | VmHWM | 압축비 | msgs/s | startup→result |
|------|-----------|-------|--------|--------|----------------|
| in-memory (현재) | 매 시작 재학습 | 19.4 MB | 5.55× | 445 | 재학습 비용 포함 |
| **mmap (frozen)** | `.bcb-prior` 로드 | 17.4 MB | **5.55× 동일** | 432 | **즉시** |
| MCU 빌드 (in-memory) | 매 시작 재학습 | **5.97 MB** | 4.70× | 569 | 재학습 비용 포함 |

큰 prior(300KB diverse 학습, 428MB 파일)에서 시작 지연 차이:

| 모드 | startup→첫 결과 | VmHWM | 압축비 |
|------|------------------|-------|--------|
| in-memory (300KB 재학습) | 3.74 s | 495 MB | 2.98× |
| **mmap (로드)** | **0.028 s** | 431 MB | 2.98× 동일 |

mmap 의 핵심 이득은 **단일 프로세스 RSS 절감이 아니라** (a) 학습 재실행 제거로 인한
즉시 시작(prior 가 클수록 효과 큼), (b) 프로세스 간 page 공유, (c) capacity 미과할당이다.

### 원 설계 목표 vs 실측 (둘 다 병기)

| 항목 | 원 설계 목표 | 실측 |
|------|-------------|------|
| 압축비 | 2.71× 동일 | **동일**(비트 단위) ✓ |
| RAM | <5 MB | mmap 17.4 MB / **MCU 5.97 MB** — mmap 단독으론 미달 |
| msgs/s | (측정 필요) | mmap 432, MCU 569, in-memory 445 |

## 설계

### frozen 모드

`cec_enc_byte` 는 인코딩 중 BT 를 학습(`bt->train`)한다. 읽기 전용 mmap prior 에는 쓸 수
없으므로 **frozen** 모드를 도입했다(`bt_v3_freeze(1)`): train 이 pool 을 수정하지 않고
context window 만 전진시킨다. 각 메시지는 빈 window 에서 독립 인코딩(stateless)된다
(`bt_v3_reset_window`). in-memory 비교군도 frozen 으로 두면 양쪽이 동일 분포 →
**비트 동일**. (분포 계산은 `g_ctx_pool`/`g_ctx_slot`/`g_pool`/`g_bloom` 만 읽고, 쓰기 전용
`g_bt_slot` 은 안 쓰므로 prior 파일에 **저장하지 않는다** — 최대 테이블 생략.)

### 파일 포맷 (`.bcb-prior`)

```
header: magic "BCBP" | version | 빌드 서명(bt_entry_sz, ctx_entry_sz, bt_max_depth,
        bloom_bits, pres) | pool_used | ctx_used | ctx_nslots | bloom_bytes | win_len |
        각 섹션 offset(8B 정렬)
window  : 학습 종료 시점의 context window (BT_MAX_DEPTH 바이트)
pool    : BtEntry  × pool_used
ctx_pool: CtxEntry × ctx_used
ctx_slot: int      × ctx_nslots
bloom   : bit array (bloom_bytes)
```

빌드 서명이 불일치하면(예: MCU vs desktop, 다른 PRES) `bt_v3_attach` 가 거부한다.
LUT 는 로드 시 RAM 에 결정적으로 재생성된다.

### API

```c
int       bcb_prior_save(const char *path);   /* 현재 학습된 globals 직렬화 */
BcbPrior *bcb_prior_mmap(const char *path);   /* mmap 로드 (MADV_RANDOM) */
void      bcb_prior_close(BcbPrior *p);
int       bcb_prior_attach(BcbPrior *p);      /* btv3 를 prior 로 (frozen) */
CecBT     bcb_prior_cec_bt(BcbPrior *p);      /* attach + CecBT 반환 */
```

btv3 측 후크: `bt_v3_export` / `bt_v3_attach` / `bt_v3_detach` / `bt_v3_freeze` /
`bt_v3_reset_window` / `btv3_cec_bt_from_prior`. (원 설계의
`btv3_cec_bt_from_prior(BcbPrior*)` 는 v5→btv3 단방향 의존을 위해 `bcb_prior_cec_bt(BcbPrior*)`
로 분리했다.)

### CLI

```sh
build/bcb-prior-build train.txt prior.bcb-prior --train-size 50000
build/bcb-cli encode msg.bin out.bcb --prior prior.bcb-prior
build/bcb-cli decode out.bcb  msg.out --prior prior.bcb-prior
```

`--prior` 는 frozen v3 prior 를 mmap 한다. encode/decode 양쪽이 동일 파일을 써야 한다.

## 검증 (`make prior-equiv`)

5개 시나리오, train 50KB, 128B 메시지 × 300:

| 시나리오 | bit-identical (mem=mmap) | round-trip lossless |
|----------|---------------------------|----------------------|
| http_headers | yes | yes |
| iot_packets | yes | yes |
| mqtt_messages | yes | yes |
| log_lines | yes | yes |
| rpc_calls | yes | yes |

## MCU trade-off

진짜 sub-6MB RAM 은 mmap 이 아니라 MCU 빌드(`-DBCB_MCU`: 소형 고정 pool + 256Kbit bloom)가
달성한다(5.97MB, 4.70×). mmap 은 desktop/서버에서 **즉시 시작 + 프로세스 간 공유 + 정확한
크기**를 제공하고, ESP32 류에서는 flash 의 memory-mapped 영역에 prior 를 두는 확장 여지가
있다(현재 미구현). 두 기법은 상호 보완적이다.
