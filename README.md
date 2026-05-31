# BCB — Binary Compression by BT

**Lossless compression for small messages and bit-packed binary records that share a learned prior.**
All-integer / libm-free core (runs on MCUs), stable embeddable C API.

> **Bit-packing removes layout waste. BCB removes probability waste.**
> 비트패킹은 *칸 낭비*를 줄이고, BCB는 *확률 낭비*를 줄인다.

Author: 호시 <[jahyag@gmail.com](mailto:jahyag@gmail.com)> · Org: sjpupro-lab · License: **Proprietary (All Rights Reserved)** · API **v0.2.0**

-----

## What it is

General-purpose LZ compressors (zlib, LZ4, brotli, zstd, LZMA/emCompress) find redundancy *inside each
block*. On a 32–256 byte message there is almost none, so they break even or **inflate**. BCB takes the
opposite approach: encoder and decoder share a **pre-trained, frozen prior** (a learned model of the data,
never transmitted), and each message is range-coded against it. Where messages share structure — IoT
telemetry, industrial protocols, bit-packed game packets, HTTP headers — BCB compresses what LZ cannot.

작은 메시지엔 블록 내부 중복이 거의 없어 LZ 계열은 손익분기이거나 오히려 커진다. BCB는 인코더·디코더가
**미리 학습해 양쪽이 보유한 prior**(전송 안 함)에 각 메시지를 한 점으로 부호화한다. 공유 가능한 구조가
있는 데이터에서 LZ가 못 짜는 것을 짠다.

-----

## When to use / when not

**Use it for**

- Small structured packets (~20–512 B): IoT/sensor telemetry, Modbus, CAN, MQTT/CoAP, RPC, syslog, HTTP headers.
- **Already bit-packed** binary where field *values* are skewed (most deltas ≈ 0, enums dominated by 1–2 values).
- Environments where both ends can hold a shared prior, including small MCUs (ESP32 / RP2040).

**Don’t use it for**

- Data above ~1–2 KB → brotli/zstd/LZMA win via long-range matching.
- A single field below ~20 B → too small for *any* codec to help.
- Random, encrypted, or already-compressed data, or any case with no shareable prior (Shannon).

-----

## Results

All BCB numbers are round-trip lossless and reproducible from this repo (seed-fixed; CI re-runs them on
every change). Toolchain of record: **gcc 13.3.0 · libbrotli 1.1.0 · libzstd 1.5.5 · hpack 4.1.0 ·
Ubuntu 24.04 · x86_64.** Absolute ratios shift with corpus, skew, and codec versions.

### Real public sensor data (Intel Berkeley Lab), per-packet

Each packet compressed independently (true edge behavior). Best LZ rival shown = zlib+dict.

|integer telemetry|BCB+struct|zlib+dict|brotli+dict|zstd+dict   |
|-----------------|----------|---------|-----------|------------|
|20 B (2 readings)|**1.30×** |1.16×    |0.83×      |0.70×       |
|40 B (4)         |**2.05×** |1.61×    |0.99×      |0.95×       |
|80 B (8)         |**2.73×** |2.03×    |1.40×      |1.38×       |
|float32, 88 B (4)|**2.20×** |1.86×    |1.29×      |1.60× (zstd)|

Below ~20 B every codec inflates — that band is out of scope for all of them, not a contest.

### Already bit-packed game-style packets, per-packet

63 B packet, 7 entities, **no padding**, sub-byte fields (flags, 3-bit enums, 8/11-bit position & angle
deltas) with realistic skew. Not wasteful JSON — a hand-optimized binary packet.

|63 B bit-packed       |ratio        |                    |
|----------------------|-------------|--------------------|
|**BCB + structural**  |**2.20×**    |beats every LZ codec|
|zlib + dict           |1.28×        |                    |
|brotli + dict         |1.00×        |no gain             |
|zstd + dict           |0.92×        |inflates            |
|*control: random 64 B*|BCB **0.99×**|no false gain       |

Tight bit-packing fixes field *widths*; it leaves the skewed *value* distribution untouched. BCB reclaims
that. On random data it correctly does nothing.

### Small text-like messages (synthetic, `make msgbench-landmark`)

|scenario    |size |BCB+landmark|brotli+dict|zstd+dict|
|------------|-----|------------|-----------|---------|
|HTTP headers|256 B|**8.52×**   |7.98×      |6.98×    |
|MQTT        |64 B |**4.60×**   |2.24×      |2.10×    |
|RPC         |64 B |**4.00×**   |1.91×      |2.08×    |
|syslog      |64 B |**3.44×**   |1.95×      |1.72×    |

### Fixed-record binary (synthetic, `make structural-bench`)

binary_record 32 B **5.35×** · IoT 18 B **3.99×** · Modbus 25 B **3.67×** · CAN 16 B **3.91×** —
LZ family ~0.95× (inflates) on these.

### vs HPACK (RFC 7541), identical header blocks

|header set           |BCB (stateless)|HPACK cold|HPACK warm|
|---------------------|---------------|----------|----------|
|all (2825, avg 295 B)|5.87×          |1.99×     |**6.58×** |
|request              |6.73×          |1.88×     |**11.12×**|
|response             |**4.70×**      |2.23×     |3.67×     |

BCB wins HPACK cold-start ~3× (CDN / stateless / first request). HPACK *warm* (long-lived connection,
populated dynamic table) wins repeated requests; BCB still wins responses.

### Reproduce

```sh
sudo apt-get install -y build-essential libbrotli-dev libzstd-dev
make msgbench-landmark      # small text-like messages
make structural-bench       # fixed-record binary
python3 tools/bcb_vs_hpack.py --bcb-build build      # vs HPACK
sh tests/corpus/fetch_iot_real.sh && \
  build/bcb-realbench --corpus build/real_iot_int.corpus --record-size 10 \
    --recs-per-msg 8 --train-msgs 9000 --max-test 5000 --mode structural   # real sensor data
```

Methodology (shared-prior setup, byte definition, train/sample sizes, lossless checks) is in
[`docs/benchmarks.md`](docs/benchmarks.md) and [`docs/benchmarks_real.md`](docs/benchmarks_real.md).

-----

## Where this sits vs Oodle Network / SEGGER emCompress

- **emCompress** is the LZ family (LZMA/DEFLATE/LZ4); it builds its model *from within each block*, so it
  inflates on small per-packet records — exactly the band BCB targets.
- **Oodle Network** shares BCB’s paradigm (a pre-trained model held on both ends) but targets game servers
  with a 4–8 MB model; it is not an MCU library and has no fixed-record schema. We make **no same-data
  claim against Oodle** — that is best settled by running both on your own captures during evaluation.

Full analysis: [`docs/compare.md`](docs/compare.md).

-----

## Library / API (`include/bcb.h`)

```c
#include "bcb.h"
BcbPrior *p = bcb_prior_open("sensors.bcb-prior");        /* mmap; or _from_memory */
size_t cap = bcb_compress_bound(msg_len);                 /* allocate out[cap] */
ssize_t n  = bcb_compress(p, msg, msg_len, out, cap);     /* self-describing container */
ssize_t m  = bcb_decompress(p, out, (size_t)n, back, msg_len);   /* m == msg_len, lossless */
bcb_prior_close(p);
```

- One-shot + `BcbEncoder`/`BcbDecoder` handles; `bcb_compress_bound`, `bcb_prior_id`, `bcb_strerror`, `bcb_version`.
- **Thread-safe:** per-handle state; priors/LUTs shared read-only (same prior usable concurrently).
- **CRC32 integrity** (default on) → `BCB_ERR_CORRUPTED`; **prior-id** (SHA-256/16 B) catches prior mismatch.
- **Python bindings** (`bindings/python/`, cffi). Reference: [`docs/api.md`](docs/api.md).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build --output-on-failure        # includes round-trip lossless
cmake --install build --prefix /usr/local          # find_package(bcb) / pkg-config
```

### CLI / prior build

```sh
build/bcb-prior-build train.txt out.bcb-prior --train-size 50000          # base BT prior
build/bcb-prior-build train.txt out.bcb-prior --landmark-k 512            # + landmark (text-like)
build/bcb-prior-build train.bin out.bcb-prior --schema-record-size 18     # structural (binary)
build/bcb-cli encode msg.bin out.bcb --prior out.bcb-prior
build/bcb-cli decode out.bcb msg.out --prior out.bcb-prior
```

-----

## How it works

A learned prior is a model of `context → next-byte distribution` (up to 24-byte context). Both ends hold
it, so it costs zero channel bytes; each message is an arithmetic/range code against it. Three prior
enhancements (all integer-quantized, lossless):

- **landmark** — sharper distributions on frequent contexts (text-like messages).
- **structural** — position-aware byte/delta distributions for fixed-layout records (IoT/binary).
- **mmap prior** — shared prior file → instant start, no retraining (300 KB prior: 3.74 s → 0.028 s).

Math mapping and design rationale: [`docs/theory.md`](docs/theory.md).

-----

## Releases & security

Tagged releases build per-platform library bundles (static/shared/headers/CLI) + Python wheels, each with
a **CycloneDX SBOM + Sigstore (cosign) signature + SHA-256 checksum**, published behind a manual approval
gate. Verification and vulnerability reporting: [`SECURITY.md`](SECURITY.md).

## Versions

|stage|content                                          |result                    |
|-----|-------------------------------------------------|--------------------------|
|v0   |range coder + n-gram BT (reference)              |baseline                  |
|v3   |integer hot path, open addressing, libm-free, MCU|~28× faster, MCU 3.56 MB  |
|v4   |auxiliary distribution-blend channels            |+2.60 %                   |
|v5   |mmap prior + landmark + structural schema        |binary +57–286 %, lossless|
|v6   |stable public API, thread-safe, CRC32, prior-id  |API v0.2.0                |

## Honest limits

- Below ~20 B no codec helps; above ~1–2 KB brotli/zstd/LZMA win.
- No shareable prior, or random / already-compressed data → no advantage.
- Absolute ratios depend heavily on corpus redundancy and skew; synthetic-generator figures are an upper
  reference — real-data figures (above) are lower and the ones to quote.
- CRC32 detects corruption, not adversarial tampering.

-----

## License

**Proprietary — All Rights Reserved.** © 2026 호시 <[jahyag@gmail.com](mailto:jahyag@gmail.com)>. No use, copying, distribution,
modification, or reverse engineering without a separate written license; see [`LICENSE`](LICENSE).

**Commercial licensing / 30-day evaluation:** 호시 <[jahyag@gmail.com](mailto:jahyag@gmail.com)>.

> Earlier versions were published under the MIT License; those specific prior releases remain under their
> original terms. This notice governs the current and subsequent versions.
