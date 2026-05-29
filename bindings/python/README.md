# bcb — Python bindings

Python bindings for **BCB**, a lossless compressor for small messages and
fixed-record binary that share a learned *prior* (edge IoT/MCU telemetry, RPC,
small headers). The bindings compile the BCB C core directly into a self-contained
CPython extension via [cffi](https://cffi.readthedocs.io/) (no separate shared
library to ship or `dlopen`).

> Proprietary — All Rights Reserved. See the repository `LICENSE`.
> Commercial licensing / evaluation: 호시 <jahyag@gmail.com>.

## Install

Wheels target Linux + macOS + Windows (Windows uses the Win32 file-mapping shim
in the core, not POSIX `mmap`). Build a local wheel from the repository:

```sh
# from the repo root (the C sources must be present)
BCB_REPO_ROOT="$PWD" pip install ./bindings/python
# or build a wheel:
BCB_REPO_ROOT="$PWD" pip wheel ./bindings/python -w dist/
```

Multi-platform wheels are configured with `cibuildwheel` (see `pyproject.toml`).
Upload to (Test)PyPI is intentionally **not** wired up yet.

## Usage

```python
import bcb

# A prior is built once with the `bcb-prior-build` CLI and shared by both sides.
with bcb.Prior("sensors.bcb-prior") as p:
    packet = b'{"dev":"s1","t":21,"ok":true}'
    comp = p.compress(packet)                       # CRC32 on by default
    back = p.decompress(comp, original_len=len(packet))
    assert back == packet

# In-memory prior image (e.g. shipped inside your app):
with bcb.Prior.from_bytes(open("sensors.bcb-prior", "rb").read()) as p:
    ...
```

### Toggles

```python
comp = p.compress(data, checksum=False)   # drop the 4-byte CRC for max ratio
comp = p.compress(data, prior_id=True)    # embed the 16-byte prior id
# decoding with the wrong prior then raises BcbError(BCB_ERR_PRIOR_ID_MISMATCH)
```

### Reusable encoder / decoder

```python
with bcb.Prior("p.bcb-prior") as p, p.encoder() as enc, p.decoder() as dec:
    for msg in stream:
        comp = enc.encode(msg)
        assert dec.decode(comp, len(msg)) == msg
```

### Errors

All failures raise `bcb.BcbError` (with `.code` and `.name`, e.g.
`BCB_ERR_CORRUPTED`, `BCB_ERR_OUTPUT_TOO_SMALL`, `BCB_ERR_PRIOR_ID_MISMATCH`).

## Memory & threads

- A `Prior` owns its C handle; `close()` (or the `with` block) frees it, and is
  idempotent (no double-free). `Encoder`/`Decoder` likewise free on close.
- A `Prior` is **read-only and thread-safe to share**. Each compress/decompress
  call uses its own transient state. A single `Encoder`/`Decoder` instance,
  however, must not be used concurrently from multiple threads.

## Test

```sh
BCB_REPO_ROOT="$PWD" pip install ./bindings/python
pytest bindings/python/tests -q     # builds a real prior from the repo corpus
```
