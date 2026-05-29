# BCB — Binary Compression by BT
# Copyright (c) 2026 호시 <jahyag@gmail.com>
# Licensed under the MIT License. See LICENSE.
#
# Fuzzing harnesses for the untrusted decode paths (libFuzzer + ASan/UBSan).
#
#   make            # build both harnesses
#   make run        # short time-limited run of both (RUN_SECS, default 30)
#   make repro      # replay committed regression seeds (deterministic, CI gate)
#   make seeds      # (re)generate the decompress seed corpus
#   make clean
#
# Requires clang with -fsanitize=fuzzer. Override CC=clang-XX if needed.

CC      ?= clang
ROOT    := ..
INC     := -I$(ROOT)/include -I$(ROOT)/src/v0_baseline -I$(ROOT)/src/v3_integer_bt -I$(ROOT)/src/v5_mmap_prior
SRC     := $(ROOT)/src/v6_public/bcb_api.c $(ROOT)/src/v0_baseline/ce_compress.c \
           $(ROOT)/src/v0_baseline/bt_model.c $(ROOT)/src/v3_integer_bt/btv3.c \
           $(ROOT)/src/v5_mmap_prior/bcb_prior.c
LDLIBS  := -lm

# Sanitizers: fuzzer drives coverage; address+undefined catch the bugs.
SAN     := -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
CFLAGS  ?= -std=c99 -g -O1 -Wall -Wextra
RUN_SECS ?= 30

# The library keeps persistent global LUTs and a session-lifetime prior by
# design; these harnesses target memory safety (OOB/UB), not leaks.
export ASAN_OPTIONS ?= detect_leaks=0:abort_on_error=1
export UBSAN_OPTIONS ?= print_stacktrace=1:halt_on_error=1

HARNESSES := fuzz_prior fuzz_decompress

.PHONY: all run repro seeds clean
all: $(HARNESSES)

fuzz_prior: fuzz_prior.c $(SRC)
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ $^ $(LDLIBS)

fuzz_decompress: fuzz_decompress.c $(SRC)
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ $^ $(LDLIBS)

run: $(HARNESSES)
	./fuzz_prior -max_total_time=$(RUN_SECS) -print_final_stats=1 seeds_prior regressions
	./fuzz_decompress -max_total_time=$(RUN_SECS) -print_final_stats=1 seeds_decompress

# Deterministic regression gate: replay every committed crash seed. Exits
# non-zero if any seed still crashes. Safe to run with an empty corpus.
repro: $(HARNESSES)
	@echo "== replay regression seeds =="
	./fuzz_prior seeds_prior/* regressions/prior-* 2>/dev/null || true
	./fuzz_decompress seeds_decompress/* regressions/decompress-* 2>/dev/null || true
	@echo "== replay OK (no crash) =="

# Regenerate decompress seeds: one valid prior + a few valid containers.
seeds: gen_seeds
	./gen_seeds

gen_seeds: gen_seeds.c $(SRC)
	$(CC) $(CFLAGS) -fsanitize=address,undefined $(INC) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(HARNESSES) gen_seeds *.o crash-* leak-* timeout-* oom-*
