# JSON direct-to-struct bottleneck benchmark matrix

`conflux_json_direct_struct_bench` is a focused benchmark target for finding the
real costs inside direct typed JSON decode. It intentionally avoids file I/O,
socket I/O, DOM traversal, and HTTP routing so profiles land on JSON reader,
member matching, duplicate-key, numeric, and string paths.

## Why this target exists

The broad `conflux_json_bench` remains the end-to-end JSON benchmark. This target
answers narrower questions before changing hot decode code:

- Is ordered direct-to-struct decode materially faster than reverse or shuffled
  field order?
- Does the wide-object lookup path handle declaration order, shuffled order,
  interleaved unknown fields, and tail unknown fields differently?
- What is the cost of duplicate-key detection versus `first_wins` or
  `last_wins` handling?
- Are JSON5 duplicate-key rows much more expensive than strict JSON rows?
- How much time is in common number forms and fixed numeric arrays?
- How expensive is escaped string decode compared with plain owned string decode?

## Row taxonomy

| Prefix | Isolates | Rows |
|---|---|---|
| `order/medium/*` | Small manual `JsonMembers<T>` direct-to-struct member matching | declaration, reverse, evens-then-odds, shuffled |
| `order/wide64/*` | Wide generated lookup metadata and field-order sensitivity | declaration, reverse, evens-then-odds, shuffled |
| `unknown/wide64/*` | Unknown-member scan/skip policy cost | interleaved ignore, tail ignore, interleaved reject |
| `duplicate/name/*` | Duplicate known scalar policy cost | reject, first-wins, last-wins |
| `duplicate/vector/*` | Duplicate container member overwrite/skip cost | first-wins, last-wins |
| `duplicate/unknown/*` | Duplicate unknown-member handling under ignore policy | ignore |
| `duplicate/json5/*` | JSON5 mode duplicate handling | reject, last-wins |
| `numeric/scalars/*` | Mixed integer/floating scalar number lexing | mixed forms |
| `numeric/point_cloud/*` | `vector<array<double,3>>` fixed numeric array decode | integer, fixed decimal, scientific, mixed, mixed + whitespace |
| `strings/owned/*` | Owned string decode and escape handling | plain long, escaped long |

Each row reports median `ns_per_iter`, MB/s over the input bytes, allocations per
iteration, and allocated bytes per iteration. Treat allocation counters as local
process counters: they are intended for comparing rows and patches in the same
binary, not for publishing allocator-independent memory claims.

## Suggested local workflow

```sh
cmake --preset perf-clang-libcxx
cmake --build --preset perf-clang-libcxx --target conflux_json_direct_struct_bench
PERF_BUILD_DIR="$(python3 scripts/cmake-preset-build-dir.py "$PWD" perf-clang-libcxx)"

"$PERF_BUILD_DIR/benchmarks/conflux_json_direct_struct_bench" --json --filter 'order/'
"$PERF_BUILD_DIR/benchmarks/conflux_json_direct_struct_bench" --json --filter 'duplicate/'
"$PERF_BUILD_DIR/benchmarks/conflux_json_direct_struct_bench" --json --filter 'numeric/'
"$PERF_BUILD_DIR/benchmarks/conflux_json_direct_struct_bench" --json --filter 'strings/'
```

For bottleneck attribution, run the same filters through `perf stat` and symbol
sampling. At minimum, record cycles, instructions, IPC, L1I misses, branch
misses, allocation counts, binary size, and top no-children symbols.

## Interpreting deltas

- `order/medium/declaration` versus `order/medium/shuffled` shows the cost of
  out-of-order direct-to-struct field matching in the small linear path.
- `order/wide64/declaration` versus `order/wide64/shuffled` shows whether the
  generated wide lookup path is really order-insensitive.
- `unknown/wide64/interleaved_ignore` versus `unknown/wide64/tail_ignore` shows
  whether unknown-field skips disrupt the hot known-member path.
- `duplicate/name/reject` versus `duplicate/name/last_wins` separates detection
  overhead from successful overwrite policy cost.
- `numeric/point_cloud/integers`, `fixed_decimal`, and `scientific` isolate the
  number lexing/from-chars shape before touching object/member code.
- `strings/owned/plain_long` versus `strings/owned/escaped_long` isolates the
  string body scanner and escape decode path.

P2996 reflection rows in `conflux_json_reflect_bench` mirror the medium
out-of-order and duplicate-key cases when the reflection build lane is enabled.
Use those rows after validating a manual `JsonMembers<T>` change to confirm the
same strategy also helps actual reflection-based code generation.
