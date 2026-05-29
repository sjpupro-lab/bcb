<!--
  BCB — Binary Compression by BT
  Copyright (c) 2026 호시 <jahyag@gmail.com>. Proprietary — All Rights Reserved.
-->
# Contributing to BCB

Thanks for your interest. BCB is **proprietary software** (see `LICENSE`), so
please read the contribution terms below before sending changes.

## Contribution terms (important)

BCB is offered under a proprietary license and a separate commercial license, so
the maintainer must hold the rights needed to relicense contributions. **By
submitting a contribution (PR/patch) you agree that the copyright holder may use,
modify, and relicense it (including under the commercial license).** For anything
non-trivial we may ask you to sign a Contributor License Agreement (CLA) first.
If you can't agree to this, please open an issue to discuss instead of sending code.

Report **security issues privately** per `SECURITY.md` — not as public issues/PRs.

## Build & test

```sh
# Make (Linux/macOS)
make test            # v0 round-trip
make api-test        # public API round-trip + corruption detection
make threads-test    # 8-thread concurrent encode/decode lossless
make prior-equiv     # in-memory == mmap, bit-identical + lossless
make wconv           # core -Wconversion gate (-Werror)
make msgbench        # BCB vs brotli/zstd (needs libbrotli-dev, libzstd-dev)

# CMake (Linux/macOS/Windows) — static + shared lib, tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# Fuzzing (clang)
cd fuzz && make CC=clang && make CC=clang repro
```

## What every change must satisfy

These are enforced in CI (`.github/workflows/`) and reviewed:

1. **Lossless, always.** Every codec change must keep round-trip bit-exact.
   `make test api-test threads-test prior-equiv` must pass. If you change the
   codec, confirm output is **bit-identical** for unchanged behavior (compare a
   built prior + compressed sample before/after), or document the intended
   format/version bump.
2. **No new `-Wconversion` warnings in the core** — `make wconv` (`-Werror`) must
   pass. Use explicit, meaning-preserving casts.
3. **Decode robustness.** Changes to the decoder or prior parser must keep the
   fuzz corpus green (`fuzz/`, `make repro`) — malformed input returns a negative
   `BcbStatus`, never crashes/OOB/loops.
4. **Benchmarks stay honest.** Don't cherry-pick inputs; label synthetic vs real
   numbers (`docs/benchmarks.md` vs `docs/benchmarks_real.md`). No exaggeration.

## Code style

- **C99**, matching the surrounding code (compact, comment density as-is).
- **The core is libm-free.** `src/{v0_baseline,v3_integer_bt,v5_mmap_prior,v6_public}`
  must not introduce floating point on the decode/distribution hot path or pull in
  `<math.h>` there (the offline prior-build path may use it). Keep it MCU-friendly
  (`-DBCB_MCU`).
- **Portable filenames.** Do not add files/dirs whose name is a Windows reserved
  device (`con`, `prn`, `aux`, `nul`, `com1`–`com9`, `lpt1`–`lpt9`) — they break
  `git checkout` on Windows.
- Public API changes go through `include/bcb.h` with `BCB_API` and a SemVer bump
  + `CHANGELOG.md` entry. Keep internal symbols hidden (not `BCB_API`).

## Scope

Keep PRs focused. Don't refactor unrelated code or change the architecture,
algorithm behavior, or compression ratio as a side effect of another change.
