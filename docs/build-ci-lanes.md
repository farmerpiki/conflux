# Build and CI lanes

Conflux keeps correctness instrumentation separate from performance artifacts.

## Preset Families

User-facing source/package presets are intentionally small and unsurprising:

- `core` — release build, auto-detected compiler, no tests/examples/benchmarks,
  no FetchContent downloads, no runtime dependencies.
- `json` — `core` plus JSON; prefers system xxhash when present and falls back
  to the internal seeded hash otherwise.
- `http-api` — stable HTTP application/API surface without examples or test
  dependencies.
- `web-server` — stable web-server surface with provider auto-detection for
  selected components.
- `full` — all stable components for source consumers; experimental protocols
  such as HTTP/3 still require `CONFLUX_ENABLE_EXPERIMENTAL=ON`.

Development and validation presets are separate. They intentionally opt into the
full development feature set, tests, and the compiler/runtime shape named by the
preset:

- `dev-core`, `dev-json`, and `dev-http` are auto-compiler local development
  profiles for focused component work.
- `dev-all` and `dev-exp-all` are auto-compiler local development profiles for
  the full stable and full experimental surfaces.
- `debug-*`, `tsan-*`, and `fuzz-*` are correctness and instrumentation lanes.
- `release-*`, `release-*-p5`, and `release-p2996-gcc` are optimized build and
  LTO/reflection smoke lanes, not the default consumer configure.
- `perf-*` presets build benchmark artifacts only.
- `pgo-*` presets are profile-generation/profile-use lanes.
- `release-header-artifacts` validates generated header consumers.

For a normal package-oriented configure, prefer `cmake --preset core`,
`cmake --preset json`, `cmake --preset http-api`, `cmake --preset web-server`,
or `cmake --preset full`. For library development, use the explicit compiler
presets below.

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

If CMake Python discovery stalls or selects a problematic virtual environment,
pin the interpreter explicitly with `-DPython3_EXECUTABLE=/usr/bin/python3`.
The top-level configure also restricts discovery to CPython and location-first
probing to keep agent/CI sandboxes predictable.


## Fuzz smoke lane

Use:

```sh
scripts/run-fuzz-smoke.sh
```

This lane configures `fuzz-clang-stdcxx`, builds the libFuzzer harnesses, and
runs only bounded seed-corpus CTest entries labeled `fuzz-smoke`. Normal tests
and benchmarks stay disabled in the fuzz preset, so this lane validates harness
reachability without turning fuzz builds into another full correctness matrix.
The preset intentionally uses the `json` feature set plus HTTP core/realtime
parser helpers only; it does not require the io_uring server runtime, TLS,
HTTP/2, HTTP/3, or PostgreSQL client dependencies.

Equivalent direct CTest invocation after configuring/building the preset:

```sh
ctest --preset fuzz-clang-stdcxx
# or
ctest --test-dir /tmp/conflux/fuzz-clang-stdcxx -L fuzz-smoke --output-on-failure
```

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

## DB pipeline evidence

Use a separate PostgreSQL database for DB pipeline evidence so proof runs do not
share state with the benchmark-recording database:

```sh
createdb -U postgres conflux_db_evidence

PG_TEST_CONNINFO=postgresql:///postgres?user=postgres \
PG_CONNINFO=postgresql:///conflux_db_evidence?user=postgres \
DB_PIPELINE_PRESET=release-gcc-stdcxx \
DB_PIPELINE_ARTIFACT_DIR=../evidence/db-pipeline \
scripts/db_pipeline_live_evidence.sh
```

Keep `conflux_bench` for `scripts/bench_record.sh` result storage. The DB
pipeline evidence script creates and drops its own measurement tables in the
database named by `PG_CONNINFO`.

## Install/package lane

The public component and package-target map is indexed in
`docs/component-map.md`. The install export is guarded by a cheap source-tree
check:

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
target exists, verifies that unrequested public package targets are not visible,
compiles an executable that imports the installed `conflux.types` module through
`conflux::core`, links the requested installed targets, and runs the executable
with CTest. The umbrella target is available as `conflux::conflux` when the
HTTP-server aggregate was installed; the exported aggregate target also remains
available as `conflux::umbrella`.

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
coverage. Prefer isolated component smokes such as `--components dns`,
`--components template`, or `--components pg --enable-db-smoke` when changing a
component contract; those lanes prove that only the requested component and its
dependency closure are exposed.

For CI with a separately installed prefix, set
`-DCONFLUX_PACKAGE_SMOKE_PREFIX=<install-prefix>` on a test build to add
`build/package-config-install-tree` as an installed-prefix smoke test. For CI that
should perform the fresh configure/build/install/consume flow from CTest, set
`-DCONFLUX_RUN_INSTALL_TREE_SMOKE=ON`; the default smoke feature set remains
`core` so it stays cheap and liburing-free.

## Provider Policy Lane

Use:

```sh
scripts/check-provider-policy-matrix.sh
```

This lane configures and builds representative provider-policy scenarios without
mock liburing: dependency-light `core`, JSON with system/default and internal
hash providers, web-server compression with automatic gzip selection and
`CONFLUX_GZIP_PROVIDER=ALL`, stable HTTP server with HTTP/3 gated off, and HTTP
auth with the Argon2 runtime provider. `CONFLUX_GZIP_PROVIDER=AUTO` benchmarks
all discovered gzip backends during configure when more than one backend is
available, skips the benchmark when only one backend exists, and compiles only
the selected backend by default.

The AUTO result is cached in `CONFLUX_RESOLVED_GZIP_PROVIDER`, with an internal
fingerprint covering the discovered candidate set, compiler, build type, and
C++ flags. Reconfiguring the same build tree reuses the cached selection instead
of rerunning the probes. Changing those inputs invalidates the fingerprint and
reruns selection. Use `CONFLUX_GZIP_PROVIDER=ALL` to build every discovered
backend, or set `CONFLUX_GZIP_PROVIDER=LIBDEFLATE`, `ZLIB_NG`, `ZLIB`, or `ISAL`
to pin a backend and avoid configure-time benchmarking entirely.

Other provider knobs expose their resolved result in cache variables as well:
`CONFLUX_RESOLVED_JSON_HASH_PROVIDER`, `CONFLUX_RESOLVED_BROTLI_PROVIDER`,
`CONFLUX_RESOLVED_ZSTD_PROVIDER`, `CONFLUX_RESOLVED_TLS_PROVIDER`,
`CONFLUX_RESOLVED_HTTP2_PROVIDER`, `CONFLUX_RESOLVED_HTTP3_PROVIDER`,
`CONFLUX_RESOLVED_POSTGRES_PROVIDER`, and `CONFLUX_RESOLVED_ARGON2_PROVIDER`.
Those providers do not run configure-time performance probes; AUTO resolves from
dependency availability and the explicit `*_PROVIDER` setting. Pin the matching
provider option, for example `CONFLUX_TLS_PROVIDER=OPENSSL` or
`CONFLUX_POSTGRES_PROVIDER=LIBPQ`, when packaging requires a hard failure instead
of AUTO fallback.
