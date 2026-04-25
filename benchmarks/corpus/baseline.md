# JSON Benchmark Baseline

## Post-Phase 0/1/1.5/2/7 baseline (v11 cumulative)

Measured on branch `json`, release-clang-libcxx (LTO, no sanitizers).
CPU: x86-64 workstation (Gentoo, clang 21, libc++).

Phases applied:

- v15 freeze: slow-path number classifier, lazy ObjHashTable + atomic_ref CAS
- v11 Phase 0: 24-byte Node + pre-parsed numbers (`from_chars` runs once at parse time)
- v11 Phase 1: owned input buffer, zero-copy number lexemes
- v11 Phase 1.5: builder buffer migration (`built_input` → `owned_input` on finish)
- v11 Phase 2: zero-copy strings on parse side (kRawJsonSlice on unescaped)
- v11 Phase 7: dump fast path for raw JSON slices

Phases deferred:

- v11 Phase 3 (tokenizer extraction — pure refactor; SIMD prerequisite)
- v11 Phase 4 (shared staging + Frame stack — eliminates `array_children`)
- v11 Phase 5 (dedup port; depends on Phase 4)

| Benchmark | Phase 6 | v11 cumulative | Delta vs Phase 6 | Spec threshold |
|---|---|---|---|---|
| parse/small (~4KB config) | 95.7 MB/s | 129.6 MB/s | +35% | ≥500 MB/s |
| parse/large (~1MB nested) | 88.9 MB/s | 132.1 MB/s | +49% | ≥500 MB/s |
| decode/struct-like (sv fields) | 118.8 MB/s | 141.7 MB/s | +19% | ≤2× parse cost ✓ |
| find_member/1024-member (per lookup, batch=1000) | 10.5 ns | 10.9 ns | noise | ≤1000 ns ✓ |
| array/traverse 10k numbers | 100851 ns | 31848 ns | **−68%** | — |
| builder/64-member object | 18159 ns | 17600 ns | −3% | — |
| dump/plain (no sort, no ascii_only) | 165.1 MB/s | 231.8 MB/s | +40% | ≥1000 MB/s |
| dump/sort_object_keys | 148.8 MB/s | 191.3 MB/s | +29% | — |

### Where the wins came from

| Phase | Bench impact |
|---|---|
| Phase 0 (pre-parsed numbers) | `to_i64`/`to_f64` are O(1) bit_cast; collapsed array-of-numbers traversal cost |
| Phase 1 (zero-copy number lexemes) | `JsonNumberView::lexeme()` no longer copies; parse path skips the arena append for numbers |
| Phase 2 (zero-copy strings) | Parse path skips arena copy for unescaped strings (the common case) |
| Phase 7 (raw-slice dump) | Strings with `kRawJsonSlice` skip per-byte escape scan |

## Spec status

| Threshold | Value | Status |
|---|---|---|
| parse ≥500 MB/s | ~130 MB/s | gap — requires SIMD tokenization (Phase 3 prerequisite, then SIMD follow-on) |
| find_member ≤1000 ns | **10.9 ns** | **✓ MET** (≈90× under threshold) |
| dump ≥1000 MB/s | ~232 MB/s | gap — requires vectorized escape scan; Phase 7 closed ~40% of the gap on plain bytes |

## Phase 6 baseline (for reference)

Measured at commit `bba5b97`, release-clang-libcxx (LTO, no sanitizers).

**Note on `find_member` methodology:** Phase 5 and earlier used `measure()` with
`batch=1`, timing each call individually with `steady_clock::now()`. On this system,
`CLOCK_MONOTONIC` goes through a syscall (not VDSO), adding ~5800 ns overhead per
sample — completely swamping sub-microsecond lookup times. Phase 6 introduced
`batch=1000` so the clock cost is amortised across 1000 calls. The Phase 5 "1956 ns"
figure was entirely clock overhead; the true linear-scan time was ~360 ns/lookup
(measured via tight-loop). All benchmarks with batch>1 in this table use the
corrected methodology.

| Benchmark | Phase 5 (linear) | Phase 6 (hash) | Delta |
|---|---|---|---|
| parse/small (~4KB config) | 90.6 MB/s | 95.7 MB/s | +6% |
| parse/large (~1MB nested) | 92.4 MB/s | 88.9 MB/s | −4% noise |
| decode/struct-like (sv fields) | 117.0 MB/s | 118.8 MB/s | +2% |
| find_member/1024-member (batch=1000) | ~360 ns (tight-loop) | 10.5 ns | −97% |
| array/traverse 10k numbers | 102527 ns | 100851 ns | −2% noise |
| builder/64-member object | 18158 ns | 18159 ns | unchanged |
| dump/plain | 160.7 MB/s | 165.1 MB/s | +3% |
| dump/sort_object_keys | 145.2 MB/s | 148.8 MB/s | +3% |

## Phase 5 history (for reference)

Measured at commit `5c1ea44`. Old impl at `d0b94fe`.

| Benchmark | Old impl | v7 / Phase 5 | Delta |
|---|---|---|---|
| parse/small | 119.3 MB/s | 90.6 MB/s | −24% |
| parse/large | 137.9 MB/s | 92.4 MB/s | −33% |
| decode/struct-like | 128.7 MB/s | 117.0 MB/s | −9% |
| find_member/1024-member (batch=1 — clock dominated) | 2421 ns | 1956 ns | +19% |
| array/traverse 10k numbers | 5867 ns | 102527 ns | −17× |
| dump/plain | 172.2 MB/s | 160.7 MB/s | −7% |
