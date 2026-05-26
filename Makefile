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

bench: $(BUILD)/bcb-bench
	./$(BUILD)/bcb-bench $(CORPUS)

clean:
	rm -rf $(BUILD)
