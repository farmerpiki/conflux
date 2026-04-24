# JSON Benchmark Baseline

Measured on branch `json`, commit `5c1ea44`, release-clang-libcxx (LTO, no sanitizers).
CPU: x86-64 workstation (Gentoo, clang 21, libc++).
Old impl measured at `d0b94fe` (initial commit), same preset and corpora.

## Corpus sizes

| Corpus | Size |
|---|---|
| config (small) | 3531 B |
| decode | 9048 B |
| lookup (1024-member object) | 17237 B |
| array (10000 numbers) | 48891 B |
| large (~1 MB nested) | 200040 B |

## Results vs initial impl

| Benchmark | Old (d0b94fe) | New v7 | Delta | Spec threshold |
|---|---|---|---|---|
| parse/small (~4KB config) | 119.3 MB/s | 90.6 MB/s | −24% | ≥500 MB/s |
| parse/large (~1MB nested) | 137.9 MB/s | 92.4 MB/s | −33% | ≥500 MB/s |
| decode/struct-like (sv fields) | 128.7 MB/s | 117.0 MB/s | −9% | ≤2× parse cost ✓ |
| find_member/1024-member (per lookup) | 2421 ns | 1956 ns | +19% | ≤1000 ns |
| array/traverse 10k numbers | 5867 ns | 102527 ns | −17× | — |
| builder/64-member object | — | 18158 ns | — | — |
| dump/plain | 172.2 MB/s | 160.7 MB/s | −7% | ≥1000 MB/s |
| dump/sort_object_keys | — | 145.2 MB/s | — | — |

## Analysis

**v7 regressions vs old impl:**
- Parse −24–33%: arena allocation + string copy into storage vs old zero-copy
  `string_view` into a `shared_ptr<string>` backing buffer.
- Array traversal −17×: old `as_array()` returns `span<Value const>` over a contiguous
  `shared_ptr<vector<Value>>`; v7 `elements()` walks nodes through arena index
  indirection. This is the sharpest regression and the clearest optimization target.
- Dump −7%: marginal; likely cache effects from larger arena layout.

**v7 improvements vs old impl:**
- `find_member` +19%: flat arena node storage improves cache locality vs
  pointer-chased `shared_ptr<vector<pair<string,Value>>>` tree.
- Correctness, API safety, path propagation, typed decode, builder — not measurable
  but structurally superior.

**Spec gaps (both impls):**
- parse ≥500 MB/s: requires SIMD tokenization — not present in either impl
- find_member ≤1000 ns: requires hash index — not present in either impl
- dump ≥1000 MB/s: requires vectorized escape scan — not present in either impl
