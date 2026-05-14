# Build and CI lanes

Conflux keeps correctness instrumentation separate from performance artifacts.

## Correctness lane

Use:

```sh
scripts/run-sanitizer-matrix.sh
```

This lane configures, builds, and runs tests for sanitizer/debug correctness
presets. Benchmark targets are disabled in these presets so instrumented binaries
do not get mistaken for performance data.

The default matrix is:

- `debug-clang-libcxx` — ASan + UBSan correctness.
- `debug-gcc-stdcxx` — GCC module compile/test coverage without sanitizers while
  the GCC 15 sanitizer/module ICE remains relevant.
- `tsan-clang-libcxx` — TSan correctness.
- `tsan-gcc-stdcxx` — TSan correctness.

## Performance build lane

Use:

```sh
scripts/run-perf-matrix.sh
```

This lane configures and builds benchmark binaries from `perf-*` presets only.
It asserts that the preset shape is benchmark-only, `RelWithDebInfo`, and has
ASan/UBSan/TSan disabled.

The default matrix is:

- `perf-clang-libcxx`
- `perf-gcc-stdcxx`

## Measured benchmark recording

Use `scripts/bench_record.sh` for DB-backed benchmark recording on a quiet host:

```sh
PGURI=postgres://postgres@localhost/conflux_bench \
BENCH_REPS=10 \
scripts/bench_record.sh manual-run-name
```

Do not use sanitizer or debug presets for performance conclusions. Sanitizers are
allowed for local benchmark debugging only by explicitly setting
`CONFLUX_ALLOW_SANITIZED_BENCHMARKS=ON`.
