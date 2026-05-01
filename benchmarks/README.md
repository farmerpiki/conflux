# Benchmarks

`conflux_benchmarks` is a dedicated benchmark target for focused, repeatable
in-process performance work. It is intentionally scoped to routing, header
lookup, middleware composition, and compression paths, so the numbers are not
polluted by sandbox/kernel/io_uring variability.

Build:

```sh
cmake --preset release-clang-libcxx
cmake --build --preset release-clang-libcxx --target conflux_benchmarks -j1
```

or:

```sh
cmake --preset release-gcc-stdcxx
cmake --build --preset release-gcc-stdcxx --target conflux_benchmarks -j1
```

Benchmarks are also available in the debug presets when you want easier
instrumentation or debugger access:

```sh
cmake --preset debug-clang-libcxx
cmake --build --preset debug-clang-libcxx --target conflux_benchmarks -j1
```

Run:

```sh
./build/release-clang-libcxx/benchmarks/conflux_benchmarks
./build/release-clang-libcxx/benchmarks/conflux_benchmarks --list
./build/release-clang-libcxx/benchmarks/conflux_benchmarks --filter router
./build/release-clang-libcxx/benchmarks/conflux_benchmarks --iterations 50000
./build/release-clang-libcxx/benchmarks/conflux_benchmarks --format csv
./build/release-clang-libcxx/benchmarks/conflux_benchmarks --csv   # alias for --format csv
```

## Bench binary contract

All `conflux_*bench` binaries implement a standard interface:

- `--bench-info` — prints a JSON descriptor and exits 0; used by `scripts/bench_record.sh` for auto-discovery
- `--csv` — outputs CSV in the standard format: `variant,iterations,total_ns,ns_per_iter`

`--bench-info` JSON shape:
```json
{
  "name":    "logical_bench_name",
  "parser":  "standard|strip1|tcp_parallel|file_copy",
  "configs": [
    { "name": "cfg", "extra": {}, "args": ["--iterations", "N"] }
  ]
}
```

Parsers:
- `standard` — `variant,iterations,total_ns,ns_per_iter`; recorded via `record_with_reps`
- `strip1` — same but first CSV column (config name prefix) is stripped before recording
- `tcp_parallel` — custom parser; configs sourced from `benchmarks/configs/*.json`
- `file_copy` — custom parser; configs come from `--bench-info` JSON

To add a new bench:
1. Implement `--bench-info` and `--csv` in the binary
2. Add the CMake target to `_record_targets` in `benchmarks/CMakeLists.txt`

Current benchmark groups:

- `micro/*`: small hot-path operations
- `flow/*`: full in-process request flows through the public router/middleware API

This suite is the intended baseline before HTTP client work. If network or
io_uring transport benchmarks are needed later, they should be added as
separate cases rather than mixed into these logic-path measurements.
