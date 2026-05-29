# BCB fuzzing

Coverage-guided fuzzing of the **untrusted decode paths** — the bytes that an
attacker controls when BCB is deployed: the compressed container and the
`.bcb-prior` image. Harnesses are libFuzzer + AddressSanitizer + UndefinedBehaviorSanitizer.

| Harness | Target | Untrusted input |
|---|---|---|
| `fuzz_prior` | `bcb_prior_from_memory` + downstream codec use | a `.bcb-prior` image |
| `fuzz_decompress` | `bcb_decompress` / `bcb_decode` against a trusted prior | a compressed container |

The contract under test: arbitrary input must either return a valid result or a
negative `BcbStatus` — never crash, read/write out of bounds, loop forever, or
invoke undefined behaviour.

## Requirements

- `clang` with `-fsanitize=fuzzer` (the `libclang-rt-*-dev` package provides the runtimes).

## Build & run

```sh
cd fuzz
make CC=clang             # build both harnesses
make CC=clang seeds       # (re)generate the decompress corpus + a valid prior seed
make CC=clang run         # time-limited run of both (RUN_SECS=30 by default)
make CC=clang repro       # replay the committed regression seeds (deterministic gate)
```

`seeds_prior/valid.bcb-prior` (≥2 MB, dominated by the bloom filter) is *not*
committed — `make seeds` regenerates it. `seeds_decompress/*.bin` and
`regressions/*` are small and committed.

Leak detection is disabled (`ASAN_OPTIONS=detect_leaks=0`) because the core keeps
persistent global LUTs and a session-lifetime prior by design; these harnesses
target memory safety (OOB / UB), not leaks.

## Regression corpus

`regressions/` holds minimised inputs that previously triggered a crash. They are
replayed by `make repro` and in CI (`.github/workflows/fuzz.yml`) so the same bug
cannot return. Naming: `prior-<site>-<n>` / `decompress-<site>-<n>`.

| Seed | Was | Root cause | Fix |
|---|---|---|---|
| `prior-bloom_chk_rd-1` | heap-buffer-overflow READ in `bloom_chk_rd` (btv3.c) | `prior_parse` trusted header section offsets/sizes and internal table indices without validation | load-time structural validation in `prior_parse` + `bt_v3_validate` |
