# JSON Benchmark Baseline

## Phase 6 baseline

Measured on branch `json`, release-clang-libcxx (LTO, no sanitizers).
CPU: x86-64 workstation (Gentoo, clang 21, libc++).

**Note on `find_member` methodology:** Phase 5 and earlier used `measure()` with
`batch=1`, timing each call individually with `steady_clock::now()`. On this system,
`CLOCK_MONOTONIC` goes through a syscall (not VDSO), adding ~5800 ns overhead per
sample — completely swamping sub-microsecond lookup times. Phase 6 introduced
`batch=1000` so the clock cost is amortised across 1000 calls. The Phase 5 "1956 ns"
figure was entirely clock overhead; the true linear-scan time was ~360 ns/lookup
(measured via tight-loop). All benchmarks with batch>1 in this table use the
corrected methodology.

| Benchmark | Phase 5 (linear) | Phase 6 (hash) | Delta | Spec threshold |
|---|---|---|---|---|
| parse/small (~4KB config) | 90.6 MB/s | 95.7 MB/s | +6% | ≥500 MB/s |
| parse/large (~1MB nested) | 92.4 MB/s | 88.9 MB/s | −4% noise | ≥500 MB/s |
| decode/struct-like (sv fields) | 117.0 MB/s | 118.8 MB/s | +2% | ≤2× parse cost ✓ |
| find_member/1024-member (per lookup, batch=1000) | ~360 ns (tight-loop) | 10.5 ns | **−97%** | ≤1000 ns ✓ |
| array/traverse 10k numbers | 102527 ns | 100851 ns | −2% noise | — |
| builder/64-member object | 18158 ns | 18159 ns | unchanged | — |
| dump/plain | 160.7 MB/s | 165.1 MB/s | +3% | ≥1000 MB/s |
| dump/sort_object_keys | 145.2 MB/s | 148.8 MB/s | +3% | — |

## Spec status

| Threshold | Value | Status |
|---|---|---|
| parse ≥500 MB/s | ~95 MB/s | gap — requires SIMD tokenization |
| find_member ≤1000 ns | **10.5 ns** | **✓ MET** (95× under threshold) |
| dump ≥1000 MB/s | ~165 MB/s | gap — requires vectorized escape scan |

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
