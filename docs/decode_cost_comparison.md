# BCB 기기 측 해제 비용 — emCompress / Oodle Network 대비

> 경쟁사 메모리 표기 방식(해제 작업 RAM / static RAM / 스택 / 디코더 코드 / 공유 모델)에
> 맞춰 BCB 의 **해제(decode) 측** 비용을 측정한다. 모든 BCB 값은 실제 명령 출력 근거이며,
> 측정 못 한 항목은 **"미측정"** 으로 표기한다. 가짜 숫자 없음.
>
> **측정 환경:** x86-64, gcc 13.3.0, `-O2 -DBCB_MCU`. ⚠️ **ARM(타깃 MCU) codegen 은 미측정** —
> 아래 스택·.text 는 x86-64 기준이며 ARM Thumb 에서는 다를 수 있다.

---

## 1. 한 패킷 해제 시 작업 RAM (heap / stack 분리, 실측)

### Heap

| 항목 | 크기 | 근거 |
|------|------|------|
| `BcbCodec` 핸들 (per-decode, `calloc`) | **2,144 B** | `bcb_codec_new` (`bcb_prior.c:621`), `sizeof(BcbCodec)` 측정 |
| `CecDecoder` (per-decode, `calloc`) | **56 B** | `cec_dec_new` (`ce_compress.c:126`), `sizeof` 측정 |
| 출력 버퍼 (`malloc(orig_len)`) | **= 패킷 크기** (예: 64 B) | `cec_decompress` (`ce_compress.c:188`) |
| **패킷당 transient heap 합** | **≈ 2.3 KB + 패킷** | 위 합, 디코드 후 `free` |
| 정수 LUT (CONF_LOG2 + EXP2_FRAC) | **32,768 B (32 KB)** — **1회**(프로세스 수명), 초기화 후 **읽기전용** | `build_luts` (`btv3.c:240-241`), MCU PRES=EXP2_FN=4096 ×int32 |

> LUT 은 패킷마다가 아니라 **1회** 할당되고 이후 읽기전용이다. `btv3.c` 주석상 const 베이크로
> flash 에 둘 수 있으나(단계 4 TODO) **현재는 heap(`malloc`)**.

### Stack (실측, `-fstack-usage`)

해제 1패킷의 **가장 깊은 호출 체인** 누적 스택:

| 프레임 | 스택 | 근거 (`btv3.c`/`ce_compress.c`/`bcb_api.c` `.su`) |
|--------|------|------|
| `bcb_decompress` | 48 B | |
| `decompress_core` | 128 B | |
| `cec_decompress` | 48 B | |
| `cec_dec_byte` | 1,072 B | |
| `codec_dist` | 48 B | |
| `bt_v3_distribution_r` | **31,696 B** | 지배적: `probs[256]`·`ws/wt[256]`(int64), `act_freq[24][256]`(u32, ~24KB) 등 스택 배열 |
| **peak 합** | **≈ 33,040 B (32.3 KB)** | 위 합 (x86-64) |

> ⚠️ **이 스택(32 KB)은 BCB 의 명확한 약점**이다. 분포 계산이 큰 스택 배열을 쓴다.
> SEGGER 의 "<512 B 스택"과 **두 자릿수 배 차이**다. RP2040 SRAM(264 KB) 안엔 들어가나 여유는
> 줄고, 더 작은 스택의 MCU/태스크에선 스택을 키우거나 분포 배열을 정적/축소해야 한다(미개선).
> ARM codegen 에서의 정확한 값은 **미측정**.

---

## 2. 디코더 코드(.text) 크기 — MCU 빌드 (실측, `--gc-sections` + `nm -S`)

decode 경로만 GC-링크해 남은 제품 함수들의 `.text` 합:

| 구분 | .text | 포함 함수 |
|------|-------|-----------|
| 패킷당 디코드 루프 | **≈ 4.5 KB** | `cec_dec_byte`(468) + `bt_v3_distribution_r`(1849) + `codec_dist`(460) + `decompress_core`(617) + `cec_decompress`(117) + codec/reader 글루 등 |
| 1회 prior 로드/검증 | **≈ 4.5 KB** | `prior_parse`(2266) + `sha256_block`(1072) + `bt_v3_validate`(437) + `build_luts`(622) 등 |
| **decode-reachable 합** | **≈ 8.6 KB** | `nm -S` 실측, x86-64 `-O2 -DBCB_MCU` |

> ⚠️ x86-64 codegen 기준. **ARM Thumb-2 는 보통 더 작지만 미측정.** prior 로드 코드를 제외한
> 순수 per-packet 디코드 루프는 ~4.5 KB.

---

## 3. prior 모델 — 읽기전용, flash/PSRAM 배치

| 항목 | 값 | 근거 |
|------|----|----|
| prior 모델 (MCU 고정 빌드) | **3,735,552 B = 3.56 MB**, **고정**(코퍼스 무관) | `make meminfo` → `bcb-meminfo-mcu` `TOTAL` |
| 배치 | **읽기전용** — mmap(page 공유) 또는 `bcb_prior_from_memory`(복사·소유). **flash/PSRAM 상주 가능** | `include/bcb.h:88-90`, `bt_v3_attach` frozen(읽기전용), CI `prior-equiv` bit-identical |
| Oodle Network 대응 | Oodle 공유 모델 4–8 MB(읽기전용)과 **같은 패러다임**. BCB 3.56 MB 로 더 작은 편 | Oodle 공개 사양 |

---

## 4. 비교표 — SEGGER emCompress / Oodle Network / BCB

| 항목 | SEGGER emCompress | Oodle Network | **BCB (decode, 실측)** |
|------|-------------------|---------------|------------------------|
| 패러다임 | 블록 내부 LZ (모델 없음) | 공유 사전 모델(읽기전용) | 공유 학습 prior(읽기전용) |
| 공유 모델 크기 | 없음 | **4–8 MB** (서버/flash) | **3.56 MB** (MCU 빌드, flash/PSRAM, 읽기전용) |
| 해제 작업 RAM (heap) | **~2 KB** | (서버 측, 해당 없음) | **~2.3 KB/패킷** transient + **32 KB LUT(1회, 읽기전용·const-bake 가능)** |
| static RAM | **0** | — | prior 외 0 (LUT 는 init 후 읽기전용) |
| 해제 스택 | **<512 B** | — | **≈ 32 KB (x86-64 실측)** ⚠️ 큼 — 약점 |
| 디코더 코드(.text) | **0.5–2.1 KB** | — | **≈ 8.6 KB** (per-packet 루프 ~4.5 KB; x86-64, ARM 미측정) |
| 타깃 | 범용 임베디드 | 게임 서버↔클라 | 엣지 IoT/MCU 작은 패킷 |
| 강점 | 초소형 풋프린트 | 작은 패킷 고압축 | 작은 패킷 고압축 + 무압축 대비 배포 가능 |

### 정직한 결론

- **모델 크기**: BCB 3.56 MB 는 Oodle(4–8 MB)보다 작고 같은 "공유 읽기전용 모델" 계열. emCompress 는
  모델 자체가 없어(블록 내부 LZ) 작은 패킷에서 압축이 안 되는 게 트레이드오프.
- **해제 작업 RAM/코드**: emCompress 의 "2KB RAM / <512B 스택 / 0.5–2.1KB 코드"는 BCB 가 **못 맞춘다.**
  BCB heap(~2.3KB/패킷)은 비슷하나, **스택(~32KB)과 디코더 코드(~8.6KB)는 emCompress 보다 크다.**
  이는 BCB 가 큰 공유 모델 + 분포 계산을 쓰는 설계의 대가다.
- **미측정/미개선(정직히)**:
  - ARM(타깃 MCU) codegen 의 스택·.text — x86-64 만 측정.
  - 32 KB 분포 스택·32 KB LUT 의 flash/static 이전(축소·const-bake) — 가능하나 미구현.
  - 실제 MCU 보드 on-device end-to-end (전력·지연) — 미측정.

> 한 줄: **BCB 는 "Oodle 형 공유 모델(3.56MB, 읽기전용)로 작은 패킷을 고압축"하는 쪽**이지,
> **"emCompress 형 초소형 해제 풋프린트"가 아니다.** 모델은 더 작게, 해제 스택/코드는 더 크게 가는
> 트레이드오프이며, 스택 32 KB 는 개선 여지가 있는 알려진 비용이다.

---

## 재현 명령

```sh
# 스택 (peak decode chain)
cc -std=c99 -O2 -DBCB_MCU -fstack-usage -Isrc/v0_baseline -Isrc/v3_integer_bt \
   -c src/v3_integer_bt/btv3.c -o /tmp/btv3.o   # → btv3.su: bt_v3_distribution_r 31696
# 디코더 .text (gc-sections 후 decode-reachable 함수 합)
#   decode-only consumer 를 --gc-sections 로 링크 후 nm -S 합산
# prior 모델 / 무손실
make meminfo        # bcb-meminfo-mcu TOTAL 3.56 MB
make api-test       # one-shot/encoder/from_memory round-trip lossless + corruption
make prior-equiv    # in-memory == mmap bit-identical
```
