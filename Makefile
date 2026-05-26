# BCB — Binary Compression by BT
# Copyright (c) 2026 호시 <jahyag@gmail.com>
# Licensed under the MIT License. See LICENSE.

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
V4_SRC  := $(V4_DIR)/aux.c

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

.PHONY: all test bench clean msgbench msgbench-md msgbench-check

all: $(BUILD)/bcb-cli $(BUILD)/bcb-bench

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bcb-cli: tools/bcb-cli.c $(V0_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) -o $@ $^ $(LDLIBS)

$(BUILD)/bcb-bench: tools/bcb-bench.c $(V0_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) -o $@ $^ $(LDLIBS)

$(BUILD)/test_roundtrip: tests/test_roundtrip.c $(V0_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) -o $@ $^ $(LDLIBS)

$(BUILD)/test_v1_compare: tests/test_v1_compare.c $(V0_SRC) $(V1_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V1_INC) -o $@ $^ $(LDLIBS)

test: $(BUILD)/test_roundtrip
	./$(BUILD)/test_roundtrip

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

# ── 작은 메시지 벤치마크 ─────────────────────────────────
$(BUILD)/bcb-msgbench: tools/bcb-msgbench.c $(V0_SRC) $(V3_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V3_INC) -o $@ $^ $(LDLIBS) $(MSGBENCH_LIBS)

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

# CI 회귀: BCB 압축비를 committed baseline 과 비교 (±2% 허용)
msgbench-check: $(BUILD)/bcb-msgbench $(addprefix $(BUILD)/corpus_,$(addsuffix .bin,$(SCENARIOS)))
	python3 $(SCN)/check_regression.py --bin $(BUILD)/bcb-msgbench \
	  --build $(BUILD) --train-size $(MSG_TRAIN) --message-sizes $(MSG_SIZES) \
	  --samples $(MSG_SAMPLES) --baseline $(SCN)/baseline.json --tol 0.02

clean:
	rm -rf $(BUILD)
