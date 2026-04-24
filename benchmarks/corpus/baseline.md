# JSON Benchmark Baseline

Measured on branch `json`, commit `5c1ea44`, release-clang-libcxx (LTO, no sanitizers).
CPU: x86-64 workstation (Gentoo, clang 21, libc++).

## Corpus sizes

| Corpus | Size |
|---|---|
| config (small) | 3531 B |
| decode | 9048 B |
| lookup (1024-member object) | 17237 B |
| array (10000 numbers) | 48891 B |
| large (~1 MB nested) | 200040 B |

## Results

| Benchmark | Median | Throughput | Spec threshold | Gap |
|---|---|---|---|---|
| parse/small (~4KB config) | 37155 ns | 90.6 MB/s | ≥500 MB/s | 5.5× below |
| parse/large (~1MB nested) | 2065625 ns | 92.4 MB/s | ≥500 MB/s | 5.4× below |
| decode/struct-like (sv fields) | 73753 ns | 117 MB/s | ≤2× parse cost | ✓ |
| find_member/1024-member (per lookup) | 1956 ns | — | ≤1000 ns | 2× above |
| array/traverse 10k numbers | 102527 ns | — | — | — |
| builder/64-member object | 18158 ns | — | — | — |
| dump/plain | 20952 ns | 160.7 MB/s | ≥1000 MB/s | 6.2× below |
| dump/sort_object_keys | 23187 ns | 145.2 MB/s | — | — |

## Notes

- parse ≥500 MB/s spec target assumes SIMD-assisted tokenization; current impl is scalar
  recursive-descent — gap is expected and deferred
- find_member 2× above target; current impl is linear scan — hash index deferred
- dump ≥1000 MB/s gap consistent with scalar string escaping — deferred
- decode ≤2× parse cost met (117 MB/s decode vs 90.6 MB/s parse with sv fields)
