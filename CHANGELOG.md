<!--
  BCB — Binary Compression by BT
  Copyright (c) 2026 호시 <jahyag@gmail.com>. Proprietary — All Rights Reserved.
-->
# Changelog

All notable changes to BCB are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the public C API
(`include/bcb.h`) follows [Semantic Versioning](https://semver.org/).

The version that SemVer tracks is the **public API** (`bcb_version()` →
`"0.2.0"`). The `v0`–`v6` labels below are internal development stages of the
codec, not API releases.

## [Unreleased]

### Added
- **CMake build** (`CMakeLists.txt`): static **and shared** library
  (`libbcb.a` / `libbcb.so`/`.dll`/`.dylib`), CLI tools, tests, `install` rules,
  pkg-config (`bcb.pc`), and `find_package(bcb)` → `bcb::bcb`. Shared library
  exports only the public API (hidden visibility); `BCB_API` macro in `bcb.h`.
- **Windows support**: Win32 file-mapping shim (`CreateFileMapping`/`MapViewOfFile`)
  for the prior loader, MSVC build, and a `windows` CI job.
- **Python bindings** (`bindings/python/`, cffi): `Prior`/`Encoder`/`Decoder`,
  `compress`/`decompress`, `BcbError`; `pyproject.toml` + cibuildwheel config.
- **Fuzzing** (`fuzz/`): libFuzzer + ASan/UBSan harnesses for the decode path and
  the untrusted prior parser, with a committed regression corpus and CI job.
- **Release automation** (`.github/workflows/release.yml`): tag-driven signed
  (Sigstore) releases with CycloneDX SBOMs and checksums; gated PyPI publish.
- **Standard project docs**: `SECURITY.md`, `CONTRIBUTING.md`,
  `CODE_OF_CONDUCT.md`, `CHANGELOG.md`, README Quickstart.
- **Real-data benchmarks** (`docs/benchmarks_real.md`): honest evaluation on real
  HTTP/2 headers and Intel Lab IoT telemetry (edge per-message vs gateway batch).

### Changed
- **License: MIT → proprietary (All Rights Reserved).** Earlier MIT releases
  remain available under their original terms; this applies to current and
  subsequent versions. Commercial EULA + subscription drafts in `docs/commercial/`
  (pending legal review). See `LICENSE`.

### Fixed
- **Untrusted prior hardening**: `prior_parse` + `bt_v3_validate` now bounds-check
  every section and validates internal indices / chain termination, rejecting
  malformed `.bcb-prior` images instead of risking out-of-bounds reads (found by
  fuzzing).
- **All 51 `-Wconversion` warnings** removed from the core (explicit casts; output
  bit-identical), with a `make wconv` CI regression gate.
- Renamed `aux.c`/`aux.h` → `aux_channel.*` (`aux` is a reserved name on Windows
  and broke `git checkout` there).

## [0.2.0]

### Added
- Stable public C library API (`include/bcb.h`, `libbcb.a`): one-shot
  `bcb_compress`/`bcb_decompress`, reusable `BcbEncoder`/`BcbDecoder`,
  `bcb_compress_bound`, `bcb_strerror`, `bcb_version`, `BcbStatus` error codes.
- **Thread safety**: per-instance codec state; read-only shared prior + LUTs.
- **CRC32 integrity** (on by default) → `BCB_ERR_CORRUPTED` on damage.
- **Prior id** (SHA-256 prefix) embedding → `BCB_ERR_PRIOR_ID_MISMATCH`.

## [0.1.0] — development stages (v0–v6)
- **v0** baseline: range coder + n-gram BT.
- **v1** symmetric distribution (sum = 1 normalization).
- **v3** integer hot path (log-domain LUTs, open addressing, libm-free, MCU build).
- **v4** auxiliary channel (distribution blend).
- **v5** mmap prior + landmark prior + structural (position-aware) schema.
- **v6** public library packaging.

(The v2 clock-hierarchy experiment was measured and dropped; see
`docs/benchmarks_legacy.md`.)
