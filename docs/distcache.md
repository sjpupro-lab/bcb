# BT distribution cache (`codec_dist` memoization)

**One-line:** memoize the per-byte BT distribution (`bt_v3_distribution_r`) keyed on
the context window. Frozen prior ⇒ `window → cum` is a *pure function*, so this is
**bit-identical** (0 % ratio change) — the whole reason it can exist. Default on;
compile out with `-DBCB_NO_DISTCACHE`.

## Why

PR #48's byte-rc measurement located the per-byte cost in the *distribution
recompute* (`codec_dist → bt_v3_distribution_r`, a 256-symbol blend over the active
contexts), not the range coder. PR1/landmark already skip that recompute for the
contexts they cover, via precomputed `cum`. This extends the same idea to the
general BT path: instead of a static precompute (impossible — unbounded context
combinations) it **runtime-memoizes** `(window → cum)` and reuses it when the same
window recurs.

With a frozen prior, `train` only slides the window; it never mutates the prior.
So `bt_v3_distribution_r(window)` always returns the same `cum`. Memoizing it is
therefore exact: a hit returns precisely what the recompute would have, and a
**full-key compare on every hit** (window bytes + length + scale) means a hash
collision can never return a wrong `cum` — it just recomputes. This is the opposite
trade-off from byte-rc: **no compression-ratio loss by construction.**

## Design

- **Scope:** only the BT *miss* path. Landmark hits (precomputed `cum`) and the
  structural path bypass the cache entirely — unchanged.
- **Per-instance / thread-safe:** the cache lives in `BcbCodec` (no global state),
  so concurrent handles are safe (verified by `threads-test`, 8 threads).
- **Structure:** direct-mapped (1-way) table, `BCB_DISTCACHE_SLOTS` = 512 (power of
  two; override at compile time). Key = context window contents + length + scale
  (FNV-1a hash → slot, full `memcmp` to confirm). Value = `cum[257]`. ~1 KB/entry,
  ~545 KB/codec (BT mode only).
- **Reset:** warm across messages by default. The window resets identically at each
  message boundary, so a frozen prior gives the same `window → cum` mapping across
  messages — keeping entries alive is bit-identical *and* is where the speedup comes
  from (window keys recur across messages, not within one ≤512 B message). A strict
  per-message clear is available via `-DBCB_DISTCACHE_PER_MSG_CLEAR` (O(1) generation
  bump) but it discards almost all hits (≈0 % at 128 B) and gains nothing.
- **Toggle:** `-DBCB_NO_DISTCACHE` removes the struct fields, the lookup, and the
  memory entirely — byte-identical output, no footprint growth (for MCU/flash-tight
  targets).

## Bit-identical (the absolute constraint)

`make distcache-test` builds the codec path twice (cache on vs `-DBCB_NO_DISTCACHE`)
and `cmp`s the concatenated range-coder payloads over every scenario:

```
http_headers     bit-identical=yes
iot_packets      bit-identical=yes
mqtt_messages    bit-identical=yes
log_lines        bit-identical=yes
rpc_calls        bit-identical=yes
```

Plus the unit test proves correctness from the other direction: a lossless
round-trip over a warm-cache run with a non-zero hit count — the range coder is
bit-sensitive, so any wrong cached `cum` (e.g. a bad collision) would corrupt the
decode. It also checks cold-cache and warm-cache encodings of the same message are
byte-identical.

## Throughput & hit% (`make distcache-bench`, 128 B msgs, 500 msgs × 100 reps)

Per-instance codec path (the path the public API ships). `Δ` = cache-on vs
`-DBCB_NO_DISTCACHE`. Ratio identical in every row.

| scenario      | prior   | hit% | enc Δ      | dec Δ      | bit-identical |
|---------------|---------|------|------------|------------|---------------|
| http_headers  | landmark| 62.9 | **+17.4 %**| **+18.2 %**| yes |
| http_headers  | no-lm   | 57.2 | **+15.0 %**| **+18.4 %**| yes |
| rpc_calls     | landmark| 60.1 | +1.7 %     | +1.0 %     | yes |
| rpc_calls     | no-lm   | 54.9 | **+6.0 %** | **+4.9 %** | yes |
| iot_packets   | landmark| 52.5 | −2.7 %     | −1.1 %     | yes |
| iot_packets   | no-lm   | 52.5 | −0.7 %     | −1.4 %     | yes |
| mqtt_messages | landmark| 57.4 | −2.8 %     | −2.6 %     | yes |
| mqtt_messages | no-lm   | 52.1 | −3.1 %     | +1.2 %     | yes |
| log_lines     | landmark| 56.6 | −2.0 %     | −5.6 %     | yes |
| log_lines     | no-lm   | 52.9 | −1.9 %     | +0.3 %     | yes |

**Reading it:** the cache wins big where the recompute it replaces is *expensive* —
http_headers' long, repeated field-name contexts (`Content-Type:`, `Accept:` …)
recur across messages and blend many active contexts, so a hit (one `memcpy`)
replaces real work: **+17 %**. rpc_calls is similar but milder (**+5 %** no-lm).
Where the recompute is already cheap post-#48 (iot/mqtt/log), the hash + `memcmp` +
1 KB store-on-miss overhead slightly exceeds the saving (−2 to −5 %), even at the
same ~52–57 % hit rate. Hit% is high everywhere; the throughput swing tracks the
*cost per recompute*, not the hit rate.

This is the "landmark-weak structured text" case the spec called out as the
intended beneficiary: http_headers/rpc are exactly where it pays.

## Footprint

~545 KB per BT-mode codec (512 × `cum[257]` + key). Structural-mode codecs allocate
nothing. `-DBCB_NO_DISTCACHE` → 0 growth (verified: the struct fields and table are
`#ifdef`-gated out).
