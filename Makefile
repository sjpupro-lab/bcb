# BCB — Binary Compression by BT
# Copyright (c) 2026 호시 <jahyag@gmail.com>
# Proprietary — All Rights Reserved. See LICENSE.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
LDLIBS  := -lm

V0_DIR  := src/v0_baseline
V0_INC  := -I$(V0_DIR)
V0_SRC  := $(V0_DIR)/ce_compress.c $(V0_DIR)/bt_model.c

V1_DIR  := src/v1_symmetric_dist
V1_INC  := -I$(V1_DIR)
V1_SRC  := $(V1_DIR)/symdist.c

V3_DIR  := src/v3_integer_bt
V3_INC  := -I$(V3_DIR)
V3_SRC  := $(V3_DIR)/btv3.c

V4_DIR  := src/v4_aux_channel
V4_INC  := -I$(V4_DIR)
V4_SRC  := $(V4_DIR)/aux_channel.c

V5_DIR  := src/v5_mmap_prior
V5_INC  := -I$(V5_DIR)
V5_SRC  := $(V5_DIR)/bcb_prior.c

V6_DIR  := src/v6_public
V6_INC  := -Iinclude
V6_SRC  := $(V6_DIR)/bcb_api.c

BUILD   := build
CORPUS  := tests/corpus/pride_and_prejudice.txt

# msgbench: 작은 메시지 벤치 (BCB vs brotli+dict vs zstd+dict).
# 요구: libbrotli-dev (libbrotlienc/dec/common), libzstd-dev.
MSGBENCH_LIBS := -lbrotlienc -lbrotlidec -lbrotlicommon -lzstd
SCN      := tests/scenarios
SCENARIOS := http_headers iot_packets mqtt_messages log_lines rpc_calls
MSG_TRAIN ?= 50000
MSG_SIZES ?= 64,128,256,512,1024,2048,4096
MSG_BYTES ?= 400000
MSG_SAMPLES ?= 24

.PHONY: all test bench clean msgbench msgbench-md msgbench-landmark msgbench-check prior prior-equiv prior-rss hpack hpack-docs landmark landmark-verify landmark-bench landmark-bench-docs structbench structural-bench structural-verify api-test threads-test nodiv-test

all: $(BUILD)/bcb-cli $(BUILD)/bcb-bench

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bcb-cli: tools/bcb-cli.c $(V0_SRC) $(V3_SRC) $(V5_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) $(V5_INC) -o $@ $^ $(LDLIBS)

$(BUILD)/bcb-bench: tools/bcb-bench.c $(V0_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) -o $@ $^ $(LDLIBS)

$(BUILD)/test_roundtrip: tests/test_roundtrip.c $(V0_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) -o $@ $^ $(LDLIBS)

$(BUILD)/test_v1_compare: tests/test_v1_compare.c $(V0_SRC) $(V1_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V1_INC) -o $@ $^ $(LDLIBS)

test: $(BUILD)/test_roundtrip
	./$(BUILD)/test_roundtrip

# BCB_MCU_NO_DIV divider unit test (no-HW-divide range coder). Includes
# ce_compress.c directly to reach the static shift-subtract divider.
$(BUILD)/test_nodiv: tests/test_nodiv.c $(V0_DIR)/ce_compress.c $(V0_DIR)/ce_compress.h | $(BUILD)
	$(CC) $(CFLAGS) -DBCB_MCU_NO_DIV $(V0_INC) -o $@ tests/test_nodiv.c $(LDLIBS)

nodiv-test: $(BUILD)/test_nodiv
	./$(BUILD)/test_nodiv

# v1 합=1 강제 ablation (v0 vs v1a)
v1-compare: $(BUILD)/test_v1_compare
	./$(BUILD)/test_v1_compare $(CORPUS)

$(BUILD)/test_v4_aux: tests/test_v4_aux.c $(V0_SRC) $(V1_SRC) $(V3_SRC) $(V4_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V1_INC) $(V3_INC) $(V4_INC) -o $@ $^ $(LDLIBS)

# v4 byte_type distribution blend ablation (baseline vs +aux, 4권)
v4-aux: $(BUILD)/test_v4_aux
	./$(BUILD)/test_v4_aux

$(BUILD)/test_v3_compare: tests/test_v3_compare.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS)

# v3 distribution caching: v0 vs v3 (동등성·압축비·속도, 4권)
v3-compare: $(BUILD)/test_v3_compare
	./$(BUILD)/test_v3_compare

$(BUILD)/test_v3_scale: tests/test_v3_scale.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS)

# v3 open addressing 대규모 학습 스케일링 (v0 vs v3 학습시간)
v3-scale: $(BUILD)/test_v3_scale
	./$(BUILD)/test_v3_scale

$(BUILD)/bcb-meminfo: tools/bcb-meminfo.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS)

$(BUILD)/bcb-meminfo-mcu: tools/bcb-meminfo.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -DBCB_MCU $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS)

# v3 메모리 footprint + 무손실 점검: 데스크톱 / MCU 두 설정
meminfo: $(BUILD)/bcb-meminfo $(BUILD)/bcb-meminfo-mcu
	./$(BUILD)/bcb-meminfo
	@echo ""
	./$(BUILD)/bcb-meminfo-mcu

$(BUILD)/test_v3_pool: tests/test_v3_pool.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS)            # 동적(기본)
$(BUILD)/test_v3_pool8: tests/test_v3_pool.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -DBCB_POOL_BITS=23 $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS)
$(BUILD)/test_v3_pool32: tests/test_v3_pool.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -DBCB_POOL_BITS=25 $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS)
$(BUILD)/test_v3_pool64: tests/test_v3_pool.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -DBCB_POOL_BITS=26 $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS)

# BT_POOL 비교: 고정 8M/32M/64M vs 동적. large.txt 필요(fetch_large.sh).
v3-pool: $(BUILD)/test_v3_pool8 $(BUILD)/test_v3_pool32 $(BUILD)/test_v3_pool64 $(BUILD)/test_v3_pool
	@echo "== fixed 8M =="; ./$(BUILD)/test_v3_pool8
	@echo "\n== fixed 32M =="; ./$(BUILD)/test_v3_pool32
	@echo "\n== fixed 64M =="; ./$(BUILD)/test_v3_pool64
	@echo "\n== dynamic =="; ./$(BUILD)/test_v3_pool

bench: $(BUILD)/bcb-bench
	./$(BUILD)/bcb-bench $(CORPUS)

# ── v5 mmap prior ────────────────────────────────────────
$(BUILD)/bcb-prior-build: tools/bcb-prior-build.c $(V0_SRC) $(V3_SRC) $(V5_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) $(V5_INC) -o $@ $^ $(LDLIBS)

$(BUILD)/bcb-prior-test: tools/bcb-prior-test.c $(V0_SRC) $(V3_SRC) $(V5_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) $(V5_INC) -o $@ $^ $(LDLIBS)

$(BUILD)/bcb-prior-test-mcu: tools/bcb-prior-test.c $(V0_SRC) $(V3_SRC) $(V5_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -DBCB_MCU $(V0_INC) $(V3_INC) $(V5_INC) -o $@ $^ $(LDLIBS)

PRIOR_TRAIN ?= 50000
PRIOR_MSG_SIZE ?= 128
PRIOR_MSGS ?= 500

# 5개 시나리오: in-memory vs mmap 비트 동일 + round-trip 무손실
prior-equiv: $(BUILD)/bcb-prior-test $(addprefix $(BUILD)/corpus_,$(addsuffix .bin,$(SCENARIOS)))
	@fail=0; for s in $(SCENARIOS); do \
	  ./$(BUILD)/bcb-prior-test equiv $(BUILD)/corpus_$$s.bin \
	    --train-size $(PRIOR_TRAIN) --msg-size $(PRIOR_MSG_SIZE) --msgs $(PRIOR_MSGS) || fail=1; \
	done; [ $$fail -eq 0 ] || { echo "PRIOR EQUIV FAILED"; exit 1; }

# RAM(in-memory) vs mmap 피크 RSS + 처리량 (http_headers 시나리오, 별도 프로세스)
prior-rss: $(BUILD)/bcb-prior-build $(BUILD)/bcb-prior-test $(BUILD)/bcb-prior-test-mcu $(BUILD)/corpus_http_headers.bin
	@C=$(BUILD)/corpus_http_headers.bin; P=$(BUILD)/http.bcb-prior; \
	./$(BUILD)/bcb-prior-build $$C $$P --train-size $(PRIOR_TRAIN); \
	ls -l $$P | awk '{printf "prior file: %s bytes\n",$$5}'; \
	./$(BUILD)/bcb-prior-test     rss-mem  $$C    --train-size $(PRIOR_TRAIN) --msg-size $(PRIOR_MSG_SIZE) --msgs $(PRIOR_MSGS); \
	./$(BUILD)/bcb-prior-test     rss-mmap $$C $$P --train-size $(PRIOR_TRAIN) --msg-size $(PRIOR_MSG_SIZE) --msgs $(PRIOR_MSGS); \
	./$(BUILD)/bcb-prior-test-mcu rss-mem  $$C    --train-size $(PRIOR_TRAIN) --msg-size $(PRIOR_MSG_SIZE) --msgs $(PRIOR_MSGS)

prior: prior-equiv prior-rss

# ── HPACK 비교 (작업 #4) ─────────────────────────────────
# 요구: python3 -m pip install hpack
$(BUILD)/bcb-blockbench: tools/bcb-blockbench.c $(V0_SRC) $(V3_SRC) $(V5_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) $(V5_INC) -o $@ $^ $(LDLIBS)

HPACK_TRAIN ?= 50000
hpack: $(BUILD)/bcb-prior-build $(BUILD)/bcb-blockbench
	python3 tools/bcb_vs_hpack.py --bcb-build $(BUILD) --train-bytes $(HPACK_TRAIN)

# 결과를 docs 로 기록
hpack-docs: $(BUILD)/bcb-prior-build $(BUILD)/bcb-blockbench
	python3 tools/bcb_vs_hpack.py --bcb-build $(BUILD) --train-bytes $(HPACK_TRAIN) \
	  --write-docs docs/hpack_comparison.md

# ── Landmark Prior Index (PR-1: 수집 + coverage 측정) ──────
$(BUILD)/bcb-landmark: tools/bcb-landmark.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS)

LM_N ?= 8
landmark: $(BUILD)/bcb-landmark $(addprefix $(BUILD)/corpus_,$(addsuffix .bin,$(SCENARIOS)))
	@for s in $(SCENARIOS); do \
	  ./$(BUILD)/bcb-landmark --corpus $(BUILD)/corpus_$$s.bin --train-size $(MSG_TRAIN) \
	    --n $(LM_N) --md --label $$s; \
	done

# ── Structural (position-aware) landmark — v6 PR-1 측정 ──────
$(BUILD)/bcb-structbench: tools/bcb-structbench.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS)

# 고정 레코드 binary 시나리오 (record-size 명시)
$(BUILD)/corpus_binary_record.bin: $(SCN)/binary_record.py | $(BUILD)
	python3 $< --bytes $(MSG_BYTES) > $@
$(BUILD)/corpus_modbus.bin: $(SCN)/modbus.py | $(BUILD)
	python3 $< --bytes $(MSG_BYTES) > $@
$(BUILD)/corpus_canbus.bin: $(SCN)/canbus.py | $(BUILD)
	python3 $< --bytes $(MSG_BYTES) > $@
$(BUILD)/corpus_iot_single.bin: $(SCN)/iot_packets.py | $(BUILD)
	python3 $< --bytes $(MSG_BYTES) --devices 1 > $@      # 단일 device stream (per-device 천장)

# 실제 코딩 바이트 — structural codec (base vs schema, round-trip 무손실)
structural-bench: $(BUILD)/bcb-prior-build $(BUILD)/bcb-blockbench
	python3 tools/structural_bench.py --build $(BUILD) --recs-per-msg 100

# ── 공개 라이브러리 (include/bcb.h) — v6 Phase 2 ──────────
# 정적 라이브러리. 제품 경로만 묶는다: v3 정수 분포 + 정수 prior + 공개 API +
# range coder. v0 reference BT(bt_model.c, bt_v4_*, exp/pow)는 CLI·테스트 전용이며
# 공개 API·인코드/디코드 핫패스가 호출하지 않으므로 라이브러리에서 제외한다.
# (range coder ce_compress.c 는 더 이상 bt_v4_* 를 참조하지 않는다 — 글루는 bt_model.c 로 이동.)
$(BUILD)/libbcb.a: $(V6_SRC) $(V0_DIR)/ce_compress.c $(V3_SRC) $(V5_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V6_INC) $(V0_INC) $(V3_INC) $(V5_INC) -c $(V6_SRC) -o $(BUILD)/bcb_api.o
	$(CC) $(CFLAGS) $(V0_INC) -c $(V0_DIR)/ce_compress.c -o $(BUILD)/ce_compress.o
	$(CC) $(CFLAGS) $(V3_INC) $(V0_INC) -c $(V3_DIR)/btv3.c -o $(BUILD)/btv3.o
	$(CC) $(CFLAGS) $(V5_INC) $(V3_INC) $(V0_INC) -c $(V5_DIR)/bcb_prior.c -o $(BUILD)/bcb_prior.o
	ar rcs $@ $(BUILD)/bcb_api.o $(BUILD)/ce_compress.o $(BUILD)/btv3.o $(BUILD)/bcb_prior.o

# 공개 API 테스트 (libbcb 만 링크, 내부 헤더 미사용)
$(BUILD)/test_api: tests/test_api.c $(BUILD)/libbcb.a | $(BUILD)
	$(CC) $(CFLAGS) $(V6_INC) -o $@ tests/test_api.c $(BUILD)/libbcb.a $(LDLIBS)

api-test: $(BUILD)/test_api $(BUILD)/bcb-prior-build $(BUILD)/corpus_mqtt_messages.bin $(BUILD)/corpus_log_lines.bin
	@./$(BUILD)/bcb-prior-build $(BUILD)/corpus_mqtt_messages.bin $(BUILD)/api.prior --train-size 50000 --landmark-k 256 >/dev/null 2>&1
	@./$(BUILD)/bcb-prior-build $(BUILD)/corpus_log_lines.bin $(BUILD)/api2.prior --train-size 50000 --landmark-k 256 >/dev/null 2>&1
	@tail -c 200 $(BUILD)/corpus_mqtt_messages.bin > $(BUILD)/api_msg.bin
	./$(BUILD)/test_api $(BUILD)/api.prior $(BUILD)/api_msg.bin $(BUILD)/api2.prior

# 멀티스레드 동시 encode/decode 무손실 (de-globalize 검증). landmark + structural prior 둘 다.
$(BUILD)/test_threads: tests/test_threads.c $(BUILD)/libbcb.a | $(BUILD)
	$(CC) $(CFLAGS) $(V6_INC) -o $@ tests/test_threads.c $(BUILD)/libbcb.a $(LDLIBS) -lpthread

threads-test: $(BUILD)/test_threads $(BUILD)/bcb-prior-build $(BUILD)/corpus_mqtt_messages.bin $(BUILD)/corpus_binary_record.bin
	@./$(BUILD)/bcb-prior-build $(BUILD)/corpus_mqtt_messages.bin $(BUILD)/thr_lm.prior --train-size 50000 --landmark-k 256 >/dev/null 2>&1
	@./$(BUILD)/bcb-prior-build $(BUILD)/corpus_binary_record.bin $(BUILD)/thr_sc.prior --train-size 49984 --schema-record-size 32 >/dev/null 2>&1
	@echo "[landmark prior]"; ./$(BUILD)/test_threads $(BUILD)/thr_lm.prior $(BUILD)/corpus_mqtt_messages.bin 8 4000
	@echo "[structural prior]"; ./$(BUILD)/test_threads $(BUILD)/thr_sc.prior $(BUILD)/corpus_binary_record.bin 8 4000

# CI 용 빠른 structural round-trip 무손실 (작은 train)
structural-verify: $(BUILD)/bcb-prior-build $(BUILD)/bcb-blockbench
	@fail=0; \
	for sp in "binary_record.py:32:" "iot_packets.py:18:--devices 1" "modbus.py:25:" "canbus.py:16:"; do \
	  g=$${sp%%:*}; rest=$${sp#*:}; rec=$${rest%%:*}; ga=$${rest#*:}; \
	  C=$(BUILD)/sv_$$g.bin; P=$(BUILD)/sv_$$g.prior; B=$(BUILD)/sv_$$g.blocks; \
	  python3 $(SCN)/$$g --bytes 60000 $$ga > $$C; \
	  ta=$$(( (20000 / $$rec) * $$rec )); msz=$$(( $$rec * 50 )); \
	  ./$(BUILD)/bcb-prior-build $$C $$P --train-size $$ta --schema-record-size $$rec >/dev/null 2>&1; \
	  python3 $(SCN)/chunk.py $$C --train-size $$ta --size $$msz --out $$B --max 100 >/dev/null; \
	  printf "%-18s " $$g; ./$(BUILD)/bcb-blockbench --prior $$P --blocks $$B 2>&1 >/dev/null | sed 's/enc_ms.*//'; \
	  ./$(BUILD)/bcb-blockbench --prior $$P --blocks $$B 2>&1 >/dev/null | grep -q 'lossless=yes' || fail=1; \
	done; [ $$fail -eq 0 ] || { echo "STRUCTURAL VERIFY FAILED"; exit 1; }

structbench: $(BUILD)/bcb-structbench $(BUILD)/corpus_iot_packets.bin $(BUILD)/corpus_iot_single.bin $(BUILD)/corpus_binary_record.bin $(BUILD)/corpus_modbus.bin $(BUILD)/corpus_canbus.bin
	@./$(BUILD)/bcb-structbench --corpus $(BUILD)/corpus_iot_packets.bin    --record-size 18 --train-size $(MSG_TRAIN) --label "iot (8-dev interleave)"
	@./$(BUILD)/bcb-structbench --corpus $(BUILD)/corpus_iot_single.bin     --record-size 18 --train-size $(MSG_TRAIN) --label "iot (single device)"
	@./$(BUILD)/bcb-structbench --corpus $(BUILD)/corpus_binary_record.bin  --record-size 32 --train-size $(MSG_TRAIN) --label binary_record
	@./$(BUILD)/bcb-structbench --corpus $(BUILD)/corpus_modbus.bin         --record-size 25 --train-size $(MSG_TRAIN) --label modbus
	@./$(BUILD)/bcb-structbench --corpus $(BUILD)/corpus_canbus.bin         --record-size 16 --train-size $(MSG_TRAIN) --label canbus

# PR-2 검증: landmark prior 무손실 + 압축비 (base vs landmark), 4 시나리오.
LM_K ?= 512
LM_MSG ?= 128
LM_VERIFY_SCN := http_headers mqtt_messages iot_packets log_lines
landmark-verify: $(BUILD)/bcb-prior-build $(BUILD)/bcb-blockbench $(addprefix $(BUILD)/corpus_,$(addsuffix .bin,$(LM_VERIFY_SCN)))
	@fail=0; for s in $(LM_VERIFY_SCN); do \
	  C=$(BUILD)/corpus_$$s.bin; B=$(BUILD)/$$s.blocks; \
	  python3 $(SCN)/chunk.py $$C --train-size $(MSG_TRAIN) --size $(LM_MSG) --out $$B >/dev/null; \
	  ./$(BUILD)/bcb-prior-build $$C $(BUILD)/$$s.base.prior --train-size $(MSG_TRAIN) 2>/dev/null; \
	  ./$(BUILD)/bcb-prior-build $$C $(BUILD)/$$s.lm.prior   --train-size $(MSG_TRAIN) --landmark-n $(LM_N) --landmark-k $(LM_K) 2>/dev/null; \
	  printf "%-15s base: " $$s; ./$(BUILD)/bcb-blockbench --prior $(BUILD)/$$s.base.prior --blocks $$B >/dev/null 2>/tmp/lm_base; cat /tmp/lm_base; \
	  printf "%-15s lm:   " $$s; ./$(BUILD)/bcb-blockbench --prior $(BUILD)/$$s.lm.prior   --blocks $$B >/dev/null 2>/tmp/lm_lm;   cat /tmp/lm_lm; \
	  grep -q 'lossless=yes' /tmp/lm_lm || fail=1; \
	done; [ $$fail -eq 0 ] || { echo "LANDMARK VERIFY FAILED (lossless)"; exit 1; }

# PR-3: base vs landmark 벤치 (압축비·처리량·random access) across sizes
landmark-bench: $(BUILD)/bcb-prior-build $(BUILD)/bcb-blockbench $(addprefix $(BUILD)/corpus_,$(addsuffix .bin,$(LM_VERIFY_SCN)))
	python3 tools/landmark_bench.py --build $(BUILD) --train $(MSG_TRAIN) --k $(LM_K) --n $(LM_N)

landmark-bench-docs: $(BUILD)/bcb-prior-build $(BUILD)/bcb-blockbench $(addprefix $(BUILD)/corpus_,$(addsuffix .bin,$(LM_VERIFY_SCN)))
	python3 tools/landmark_bench.py --build $(BUILD) --train $(MSG_TRAIN) --k $(LM_K) --n $(LM_N) \
	  --write-docs docs/landmark.md

# ── 작은 메시지 벤치마크 ─────────────────────────────────
$(BUILD)/bcb-msgbench: tools/bcb-msgbench.c $(V0_SRC) $(V3_SRC) $(V5_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) $(V5_INC) -o $@ $^ $(LDLIBS) $(MSGBENCH_LIBS)

# real-data benchmark (message-boundary aware): BCB+lm/+struct vs brotli/zstd/gzip/zlib
$(BUILD)/bcb-realbench: tools/bcb-realbench.c $(V0_SRC) $(V3_SRC) $(V5_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) $(V5_INC) -o $@ $^ $(LDLIBS) $(MSGBENCH_LIBS) -lz

# 시나리오별 합성 코퍼스 생성 (결정적, --seed 고정)
$(BUILD)/corpus_%.bin: $(SCN)/%.py | $(BUILD)
	python3 $< --bytes $(MSG_BYTES) > $@

# 5개 시나리오 자동 측정 (사람이 읽는 표)
msgbench: $(BUILD)/bcb-msgbench $(addprefix $(BUILD)/corpus_,$(addsuffix .bin,$(SCENARIOS)))
	@for s in $(SCENARIOS); do \
	  echo "=== $$s ==="; \
	  ./$(BUILD)/bcb-msgbench --corpus $(BUILD)/corpus_$$s.bin \
	    --train-size $(MSG_TRAIN) --message-sizes $(MSG_SIZES) --samples $(MSG_SAMPLES); \
	  echo ""; \
	done

# 동일 측정을 markdown 표로 (docs 갱신용)
msgbench-md: $(BUILD)/bcb-msgbench $(addprefix $(BUILD)/corpus_,$(addsuffix .bin,$(SCENARIOS)))
	@for s in $(SCENARIOS); do \
	  ./$(BUILD)/bcb-msgbench --corpus $(BUILD)/corpus_$$s.bin \
	    --train-size $(MSG_TRAIN) --message-sizes $(MSG_SIZES) --samples $(MSG_SAMPLES) \
	    --md --label $$s; \
	  echo ""; \
	done

# BCB vs BCB+landmark vs brotli vs zstd (landmark 열 포함)
MSG_LM_K ?= 512
MSG_LM_N ?= 8
msgbench-landmark: $(BUILD)/bcb-msgbench $(addprefix $(BUILD)/corpus_,$(addsuffix .bin,$(SCENARIOS)))
	@for s in $(SCENARIOS); do \
	  echo "=== $$s ==="; \
	  ./$(BUILD)/bcb-msgbench --corpus $(BUILD)/corpus_$$s.bin \
	    --train-size $(MSG_TRAIN) --message-sizes $(MSG_SIZES) --samples $(MSG_SAMPLES) \
	    --landmark-k $(MSG_LM_K) --landmark-n $(MSG_LM_N); \
	  echo ""; \
	done

# CI 회귀: BCB 압축비를 committed baseline 과 비교 (±2% 허용)
msgbench-check: $(BUILD)/bcb-msgbench $(addprefix $(BUILD)/corpus_,$(addsuffix .bin,$(SCENARIOS)))
	python3 $(SCN)/check_regression.py --bin $(BUILD)/bcb-msgbench \
	  --build $(BUILD) --train-size $(MSG_TRAIN) --message-sizes $(MSG_SIZES) \
	  --samples $(MSG_SAMPLES) --baseline $(SCN)/baseline.json --tol 0.02

clean:
	rm -rf $(BUILD)
