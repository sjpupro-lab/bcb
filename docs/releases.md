<!--
  BCB — Binary Compression by BT
  Copyright (c) 2026 호시 <jahyag@gmail.com>. Proprietary — All Rights Reserved.
-->
# Releases & supply-chain verification

Releases are automated by [`.github/workflows/release.yml`](../.github/workflows/release.yml),
triggered by pushing a semver tag (`vX.Y.Z`). `workflow_dispatch` builds the
artifacts **without** publishing (useful for dry runs).

## What a release contains

Per native platform (Linux x86_64, macOS universal, Windows x86_64) a bundle
built and installed via CMake:

```
bcb-<ver>-<platform>.tar.gz        # libbcb.a + libbcb.so/.dylib + bcb.h + CLI + cmake/pkg-config + LICENSE
bcb-<ver>-<platform>.cdx.json      # CycloneDX SBOM
bcb-<ver>-<platform>.sha256        # SHA-256 checksums
*.sig  /  *.pem                    # Sigstore (cosign) signature + certificate
```

Plus Python **wheels** (`cibuildwheel`, Linux + macOS + Windows) attached to the
release and, gated, published to PyPI.

> Windows uses the Win32 file-mapping shim in `src/v5_mmap_prior/bcb_prior.c`
> (`CreateFileMapping`/`MapViewOfFile`) instead of POSIX `mmap`. The `windows`
> CI job builds with MSVC and runs the dynamic-library round-trip test.

## Verifying (consumers)

See [`SECURITY.md`](../SECURITY.md#release-artifact-verification-supply-chain) —
checksums, then `cosign verify-blob` pinning the workflow signing identity, then
inspect the SBOM.

## One-time repository setup (maintainer)

The publish jobs are intentionally gated behind **manual-approval GitHub
Environments** so a tag push never auto-publishes:

1. **Settings → Environments → `release`**: add yourself as a *required reviewer*.
   This gates the GitHub Release upload (`publish-release`).
2. **Settings → Environments → `pypi`**: add a *required reviewer*. This gates
   `publish-pypi`.
3. **PyPI Trusted Publishing**: on PyPI, add a *pending publisher* for the `bcb`
   project bound to owner `sjpupro-lab`, repo `bcb`, workflow `release.yml`,
   environment `pypi`. No API token is stored — publishing uses OIDC.
   - For a dry run first, uncomment `repository-url: https://test.pypi.org/legacy/`
     in `publish-pypi` and register the publisher on TestPyPI instead.

With these in place, `git tag v0.2.0 && git push origin v0.2.0` builds and signs
everything, then waits for your approval before either publish step runs.

## Cutting a release

```sh
# 1) bump versions (CMakeLists.txt project(), bindings/python/pyproject.toml,
#    include/bcb.h BCB_VERSION_*), update CHANGELOG, commit, merge to main.
# 2) tag and push
git tag -a v0.2.0 -m "BCB 0.2.0"
git push origin v0.2.0
# 3) approve the `release` (and `pypi`) environments when prompted.
```
