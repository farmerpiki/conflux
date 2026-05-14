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

Do not use sanitizer or debug presets for performance conclusions. `bench_record.sh`
now enforces this by default: normal DB recordings require `perf-*` presets,
`RelWithDebInfo`, benchmark-only builds, no LTO, and no sanitizers.
`scripts/run-sanitizer-matrix.sh` independently rejects benchmark-enabled
correctness presets. For explicit experiments with release/PGO presets, set
`ALLOW_NON_PERF_BENCH_PRESET=1`; for local sanitizer benchmark debugging, also
set `CONFLUX_ALLOW_SANITIZED_BENCHMARKS=ON`. Treat either setting as a waiver,
not as valid perf evidence.
## Install/package lane

The install export is guarded by a cheap source-tree check:

```sh
ctest --test-dir <build-dir> -R build/package-config --output-on-failure
# or directly:
scripts/check-package-config.sh
```

The guard keeps the CMake package shape explicit: `project()` owns the package
version, `conflux-config-version.cmake` uses that version, installed targets are
exported under the `conflux::` namespace, requested package components are
validated, and the installed package exposes available components/targets.

After installing a build, validate the install tree with:

```sh
scripts/run-package-config-smoke.sh \
  --prefix /tmp/conflux-install \
  --components 'core;json'
```

The smoke project configures a downstream consumer with `find_package(conflux
REQUIRED COMPONENTS ...)` and verifies that each requested `conflux::<component>`
target exists. The umbrella target is available as `conflux::conflux` when the
HTTP-server aggregate was installed; the exported aggregate target also remains
available as `conflux::umbrella`.

For CI, set `-DCONFLUX_PACKAGE_SMOKE_PREFIX=<install-prefix>` on a test build to
add `build/package-config-install-tree` as an install-tree smoke test.
