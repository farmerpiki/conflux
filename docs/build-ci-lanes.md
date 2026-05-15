# Build and CI lanes

Conflux keeps correctness instrumentation separate from performance artifacts.

## Correctness lane

Use:

```sh
scripts/run-sanitizer-matrix.sh
```

This lane configures, builds, and runs tests for sanitizer/debug correctness
presets. It also asserts the preset shape before building: tests enabled,
benchmarks disabled, LTO disabled, and exactly the expected sanitizer mix for
each preset. Benchmark targets are disabled in these presets so instrumented
binaries do not get mistaken for performance data.

The default matrix is:

- `debug-clang-libcxx` — ASan + UBSan correctness.
- `debug-gcc-stdcxx` — GCC module compile/test coverage without sanitizers while
  the GCC 15 sanitizer/module ICE remains relevant.
- `tsan-clang-libcxx` — TSan correctness.
- `tsan-gcc-stdcxx` — TSan correctness.

The correctness lane includes `build/module-fragility-regression`, a cheap
source-shape check that keeps known fragile module interfaces from regrowing
`import std`, coroutine bodies, or private implementation units in public module
file sets. See `docs/module-fragility.md`.

## Performance build lane

Use:

```sh
scripts/run-perf-matrix.sh
```

This lane configures and builds benchmark binaries from `perf-*` presets only.
It asserts that the preset shape is benchmark-only, `RelWithDebInfo`, has
ASan/UBSan/TSan disabled, and keeps LTO disabled so profiles remain
symbolized and easy to attribute.

The default matrix is:

- `perf-clang-libcxx`
- `perf-gcc-stdcxx`

## Optimized release / LTO / PGO lane

Optimized presets are intentionally separate from `perf-*` presets. Use them for
ship-shape builds, LTO smoke coverage, and profile-guided optimization; do not
record benchmark evidence from them unless the run is explicitly marked as an
experiment.

CTest includes a cheap static guard:

```sh
ctest --test-dir <build-dir> -R build/optimized-presets --output-on-failure
```

The guard checks that release/PGO presets stay unsanitized, keep tests and
benchmarks enabled for smoke coverage, use explicit LTO mode where LTO is on,
and use deterministic PGO profile paths. Clang release presets use ThinLTO
because `-flto=auto` is GCC-specific and because ThinLTO keeps module-heavy
optimized builds more tractable. GCC 15 remains the no-LTO release baseline;
GCC 16 carries the GCC LTO coverage preset.

PGO workflow:

```sh
# Clang: generate raw profiles, run representative tests/workloads, merge, use.
rm -rf /tmp/conflux-pgo/clang
mkdir -p /tmp/conflux-pgo/clang
cmake --preset pgo-gen-clang-libcxx
cmake --build --preset pgo-gen-clang-libcxx -j "$(nproc)"
ctest --test-dir /tmp/conflux/pgo-gen-clang-libcxx --output-on-failure
llvm-profdata merge -output=/tmp/conflux-pgo/clang/merged.profdata \
    /tmp/conflux-pgo/clang/*.profraw
cmake --preset pgo-use-clang-libcxx
cmake --build --preset pgo-use-clang-libcxx -j "$(nproc)"

# GCC: generated profile data lives in the profile directory directly.
rm -rf /tmp/conflux-pgo/gcc16
mkdir -p /tmp/conflux-pgo/gcc16
cmake --preset pgo-gen-gcc16-stdcxx
cmake --build --preset pgo-gen-gcc16-stdcxx -j "$(nproc)"
ctest --test-dir /tmp/conflux/pgo-gen-gcc16-stdcxx --output-on-failure
cmake --preset pgo-use-gcc16-stdcxx
cmake --build --preset pgo-use-gcc16-stdcxx -j "$(nproc)"
```

## Measured benchmark recording

Use `scripts/bench_record.sh` for DB-backed benchmark recording on a quiet host:

```sh
PGURI=postgres://postgres@localhost/conflux_bench \
BENCH_REPS=10 \
scripts/bench_record.sh manual-run-name
```

Do not use sanitizer or debug presets for performance conclusions. `bench_record.sh`
now enforces this by default: normal DB recordings require `perf-*` presets,
`RelWithDebInfo`, benchmark-only builds, no LTO, and no sanitizers. For explicit
experiments with release/PGO presets, set `ALLOW_NON_PERF_BENCH_PRESET=1`; for
local sanitizer benchmark debugging, also set
`CONFLUX_ALLOW_SANITIZED_BENCHMARKS=ON`. Treat either setting as a waiver, not as
valid perf evidence.

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
REQUIRED COMPONENTS ...)`, verifies that each requested `conflux::<component>`
target exists, compiles an executable that imports the installed `conflux.types`
module through `conflux::core`, links the requested installed targets, and runs
the executable with CTest. The umbrella target is available as
`conflux::conflux` when the HTTP-server aggregate was installed; the exported
aggregate target also remains available as `conflux::umbrella`.

For a full one-command install-tree check, use:

```sh
scripts/run-install-tree-smoke.sh \
  --feature-set core \
  --components core
```

This configures a fresh dependency-light build, installs it into a temporary
prefix, then runs the downstream package smoke against that installed prefix. Use
`--feature-set json --components 'core;json'` or a larger feature preset when the
CI host has the required system dependencies and you want broader component
coverage.

For CI with a separately installed prefix, set
`-DCONFLUX_PACKAGE_SMOKE_PREFIX=<install-prefix>` on a test build to add
`build/package-config-install-tree` as an installed-prefix smoke test. For CI that
should perform the fresh configure/build/install/consume flow from CTest, set
`-DCONFLUX_RUN_INSTALL_TREE_SMOKE=ON`; the default smoke feature set remains
`core` so it stays cheap and liburing-free.
