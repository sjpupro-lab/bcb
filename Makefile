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

.PHONY: all test bench clean

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

$(BUILD)/test_v4_aux: tests/test_v4_aux.c $(V0_SRC) $(V1_SRC) $(V4_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(V0_INC) $(V1_INC) $(V4_INC) -o $@ $^ $(LDLIBS)

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

bench: $(BUILD)/bcb-bench
	./$(BUILD)/bcb-bench $(CORPUS)

clean:
	rm -rf $(BUILD)
