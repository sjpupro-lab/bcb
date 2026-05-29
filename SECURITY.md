<!--
  BCB — Binary Compression by BT
  Copyright (c) 2026 호시 <jahyag@gmail.com>. Proprietary — All Rights Reserved.
-->
# Security Policy

## Reporting a vulnerability

Please report security issues **privately** to **호시 <jahyag@gmail.com>**
(subject: `BCB SECURITY`). Do not open a public issue for undisclosed
vulnerabilities. Include a description, affected version/commit, and a minimal
reproducer (e.g. a crashing `.bcb-prior` or compressed container) if possible.

We aim to acknowledge within a few business days and to coordinate a fix and
disclosure timeline with you.

## Supported versions

Security fixes target the **latest released version** (API v0.2.x) and `main`.
Older tags are not maintained.

## Hardening already in place

- The decode paths (`bcb_decompress`/`bcb_decode`) and the untrusted `.bcb-prior`
  parser are continuously fuzzed (libFuzzer + ASan/UBSan, `fuzz/`,
  `.github/workflows/fuzz.yml`) with a committed regression corpus. A malformed
  prior is rejected at load time rather than parsed.

## Integrity vs. authenticity — important

- BCB's per-message **CRC32 is an integrity check for accidental corruption
  only**. It is **not** a defense against deliberate tampering: an attacker who
  can modify the compressed bytes can recompute the CRC. For authenticity,
  protect the data with a MAC/signature (e.g. HMAC, or transport-layer auth)
  outside BCB.
- The `prior id` (SHA-256 prefix) detects an accidental prior **mismatch**; it is
  likewise not an authentication mechanism.

## Release artifact verification (supply chain)

Releases are built by `.github/workflows/release.yml` and ship, per artifact:
a **CycloneDX SBOM** (`*.cdx.json`), a **Sigstore signature** (`*.sig` + `*.pem`
certificate), and **SHA-256 checksums** (`*.sha256`). Verify before use:

```sh
# 1) checksums
shasum -a 256 -c bcb-<ver>-<platform>.sha256

# 2) Sigstore signature (keyless). The signing identity is the GitHub Actions
#    workflow; pin it with the flags below.
cosign verify-blob \
  --certificate      bcb-<ver>-<platform>.tar.gz.pem \
  --signature        bcb-<ver>-<platform>.tar.gz.sig \
  --certificate-identity-regexp '^https://github.com/sjpupro-lab/bcb/.+' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  bcb-<ver>-<platform>.tar.gz
```

The SBOM (`*.cdx.json`) inventories the components in each bundle. This follows
the lessons of the xz/liblzma incident: prefer signed, SBOM-accompanied releases
and verify the signing identity. See `docs/releases.md` for full details.
