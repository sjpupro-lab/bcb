# BCB — Binary Compression by BT
# Copyright (c) 2026 호시 <jahyag@gmail.com>
# Proprietary — All Rights Reserved. See LICENSE.
#
# cffi out-of-line (API mode) builder. We compile the BCB core C sources directly
# into the extension module (self-contained — no separate shared lib to ship or
# dlopen). cffi was chosen over ctypes (compile-time type checking, no fragile
# runtime signature setup) and over pybind11 (BCB is plain C, no C++ needed).
#
# setuptools requires `sources` to be relative paths under setup.py's directory,
# so we vendor the needed core sources/headers (flat) into ./_csrc at build time,
# copied from the repo (BCB_REPO_ROOT, default ../..). The #include lines use
# basenames, so a flat dir + one -I resolves everything.
import os
import shutil
from cffi import FFI

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("BCB_REPO_ROOT", os.path.abspath(os.path.join(HERE, "..", "..")))
CSRC = os.path.join(HERE, "_csrc")

_CORE = [
    "include/bcb.h",
    "src/v0_baseline/ce_compress.c", "src/v0_baseline/ce_compress.h",
    "src/v3_integer_bt/btv3.c",      "src/v3_integer_bt/btv3.h",
    "src/v5_mmap_prior/bcb_prior.c", "src/v5_mmap_prior/bcb_prior.h",
    "src/v6_public/bcb_api.c",
]


def _vendor():
    """Copy the core sources/headers (flat) into ./_csrc. Self-contained build."""
    os.makedirs(CSRC, exist_ok=True)
    for rel in _CORE:
        src = os.path.join(ROOT, rel)
        if not os.path.exists(src):
            # already vendored (e.g. building from an sdist that shipped _csrc)?
            if os.path.exists(os.path.join(CSRC, os.path.basename(rel))):
                continue
            raise FileNotFoundError(
                f"missing {src}; set BCB_REPO_ROOT to the repo root")
        shutil.copy2(src, os.path.join(CSRC, os.path.basename(rel)))


_vendor()
_REL_SOURCES = [os.path.relpath(os.path.join(CSRC, os.path.basename(s)), HERE)
                for s in _CORE if s.endswith(".c")]

ffibuilder = FFI()

# Public API surface (mirror of include/bcb.h, BCB_API stripped).
ffibuilder.cdef("""
typedef enum {
    BCB_OK = 0,
    BCB_ERR_INVALID_PRIOR = -1,
    BCB_ERR_OUTPUT_TOO_SMALL = -2,
    BCB_ERR_CORRUPTED = -3,
    BCB_ERR_VERSION = -4,
    BCB_ERR_PRIOR_ID_MISMATCH = -5
} BcbStatus;

typedef struct BcbPrior BcbPrior;
typedef struct BcbEncoder BcbEncoder;
typedef struct BcbDecoder BcbDecoder;

BcbPrior *bcb_prior_open(const char *path);
BcbPrior *bcb_prior_from_memory(const void *data, size_t len);
void      bcb_prior_close(BcbPrior *p);
size_t    bcb_prior_memory_footprint(const BcbPrior *p);
int       bcb_prior_record_size(const BcbPrior *p);
const uint8_t *bcb_prior_id(const BcbPrior *p, size_t *out_len);

size_t  bcb_compress_bound(size_t input_len);
ssize_t bcb_compress(BcbPrior *p, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap);
ssize_t bcb_decompress(BcbPrior *p, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap);

BcbEncoder *bcb_encoder_new(BcbPrior *p);
ssize_t     bcb_encode(BcbEncoder *e, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap);
void        bcb_encoder_set_checksum(BcbEncoder *e, int on);
void        bcb_encoder_set_prior_id(BcbEncoder *e, int on);
void        bcb_encoder_free(BcbEncoder *e);

BcbDecoder *bcb_decoder_new(BcbPrior *p);
ssize_t     bcb_decode(BcbDecoder *d, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap);
void        bcb_decoder_free(BcbDecoder *d);

const char *bcb_strerror(BcbStatus s);
const char *bcb_version(void);
""")

ffibuilder.set_source(
    "bcb._bcb_cffi",
    '#include "bcb.h"',
    sources=_REL_SOURCES,
    include_dirs=[os.path.relpath(CSRC, HERE)],
    # BCB_STATIC: compile the API into this extension (no dllimport on Windows).
    define_macros=[("BCB_STATIC", "1")],
)

if __name__ == "__main__":
    ffibuilder.compile(verbose=True)
