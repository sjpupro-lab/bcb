# 업그레이드 사이클 변경 요약 / Upgrade-cycle Change Summary

기준 / Base: `main` `c649620` · 사이클 / Cycle: **v7 (#47·#48·#49) + #52**
무손실·호환 / Compatibility: 전부 출력 호환 (byte-identical 또는 round-trip 무손실), API v0.2.0 유지

## 1. PR 단위 요약 / Per-PR summary

| PR | 기능 / Feature | 핵심 효과 / Key effect | 무손실·호환 / Lossless | 상태 / Status |
|---|---|---|---|---|
| **#47** | 구조적 delta-symbol fast path (`BCB_TAG_SDELTA`) / structural delta-symbol fast path | 레코드형 데이터에 자리별 사전계산 cum 적용 → BT 분포 탐색 제거, 텍스트 경로 대비 **~30×** / per-position precomputed cum for record data, removes BT search, **~30×** vs text path | round-trip 무손실 / lossless round-trip | ✅ 머지 / merged |
| **#48** | btv3 분포 핫패스 최적화 / btv3 distribution hot-path opt | 미관측 byte 기여를 레벨당 1회만 계산(이전 256회) → http **2.92×**, mqtt **2.07×**, iot **1.37×** / unobserved-byte contribution hoisted out of the 256-symbol loop | **bit-identical** (바이트 동일 / byte-for-byte) | ✅ 머지 / merged |
| **#49** | `BCB_MCU_NO_DIV` — divider 없는 MCU용 / for HW without a divider | 64-bit `/` 를 shift / shift-subtract 로 대체(Cortex-M0/RV32I 등) / replaces 64-bit division with branch-free shift math | **bit-identical** (플래그 off 시 무변화 / no change when flag off) | ✅ 머지 / merged |
| **#52** | 인스턴스별 BT 분포 캐시 (distcache) / per-instance BT distribution cache | `window → cum` 메모이즈 → http **+17% enc / +18% dec**, rpc **+5%** (iot/mqtt/log −2~5%) / memoizes window→cum; default-on, opt-out `BCB_NO_DISTCACHE` | **bit-identical** · 압축비 0% 변화 / 0% ratio change | ✅ 머지 / merged |

> 4개 패치 모두 출력 호환성을 깨지 않는다 — #48·#49·#52 는 바이트 동일 스트림, #47 은 새 태그
> 추가하되 round-trip 무손실. / All four are output-compatible — #48/#49/#52 emit byte-identical
> streams, #47 adds a tag but is lossless.

## 2. 파일 단위 변경 / Per-file changes

| PR | 파일 / File | 변경 내용 / Change | +/− |
|---|---|---|---|
| #47 | `src/v5_mmap_prior/bcb_prior.c` | structural delta fast path 코어 / core | +106 / −12* |
| #47 | `src/v5_mmap_prior/bcb_prior.h` | `BCB_TAG_SDELTA` 등 선언 / declarations | +10 |
| #47 | `src/v6_public/bcb_api.c` | API 경로 연결 / API wiring | +35 |
| #48 | `src/v3_integer_bt/btv3.c` | 분포 루프 핫패스 최적화 / hot-path opt | +34 / −9 |
| #48 | `docs/spec_sheet.md` | 수치 갱신 / numbers | +34 |
| #49 | `src/v0_baseline/ce_compress.c` | `bcb_divq64` shift-subtract 분배기 / divider | +73 |
| #49 | `tests/test_nodiv.c` | no-div 동등성 테스트 / equivalence test | +61 (신규/new) |
| #49 | `Makefile` | `nodiv-test` 타깃 / target | +10 |
| #52 | `tools/bcb-distbench.c` | throughput/hit% 벤치 / bench | +174 (신규/new) |
| #52 | `tests/test_distcache.c` | 단위 + bit-identical 증명 / unit + proof | +114 (신규/new) |
| #52 | `src/v5_mmap_prior/bcb_prior.c` | 캐시 + `bcb_codec_distcache_stats()` / cache | +99 |
| #52 | `docs/distcache.md` | 설계 + 표 / design + table | +98 (신규/new) |
| #52 | `Makefile` | `distcache-test` / `distcache-bench` | +37 |
| #52 | `src/v5_mmap_prior/bcb_prior.h` | 캐시 필드 / cache fields | +6 |

\* 각 PR 머지(`git diff <merge>^1 <merge> --stat`) 기준 실측. / Measured from each merge's first-parent diff.

**합계 / Totals:** 코드+문서 4 PR · 변경 파일 13개(신규 4) · 코드 코어 변경 ~+420 라인.
관련 문서 PR(#46 spec sheet, #50 upgrade summary, #51 README rewrite)은 같은 사이클의 문서화.

## 3. 검증 / Verification (CI green)

`make test` (7/7) · `prior-equiv` 5/5 무손실 · `nodiv-test` (#49) · `distcache-test`
(bit-identical 5/5, #52) · `api-test` · `threads-test`. 압축비는 사이클 전반에서 불변.
