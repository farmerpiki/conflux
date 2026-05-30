# Project Policy

This document records the pre-v1 policy promises that are stable enough to guide
users, tests, packaging work, and downstream integrations.

## Versioning

Conflux is currently pre-v1. Public APIs may still break when the change removes
boilerplate, improves performance, simplifies the final surface, or fixes a
wrong contract before release.

After v1.0:

- patch releases keep source compatibility except for security fixes that cannot
  be made safely without a break;
- minor releases may add APIs and deprecate APIs, but do not remove previously
  supported APIs;
- major releases may remove deprecated APIs or make contract-level breaks;
- a deprecation should normally remain available for at least one minor release
  before removal;
- migration notes belong in `docs/migration/` when a change requires user code
  edits.

Pre-v1 breakage is still intentional, not accidental. Breaking proposals should
state whether the break is justified by ergonomics, performance, or both.

## Security disclosure

Until a dedicated security contact is published, report private security issues
through the repository owner's private contact channel rather than public issues.
Public issues are fine for non-sensitive hardening work and crash-only bugs that
are already disclosed.

Security-sensitive reports should include:

- affected commit or release;
- affected component, for example HTTP parser, router, static files, TLS, JSON,
  DB, DNS, or process spawning;
- reproduction input or a minimal program;
- observed impact, such as request smuggling, memory safety, data exposure,
  denial of service, path traversal, or resource exhaustion.

Expected handling policy for a release branch:

1. acknowledge the report privately;
2. reproduce and classify severity;
3. prepare a minimal fix and regression test/fuzz corpus input;
4. publish the fix with credit if the reporter wants credit;
5. add a migration or mitigation note when existing deployments need action.


## Language standard policy

Conflux should not artificially gate components behind a newer C++ standard than
the code actually needs. Standard requirements are part of each component's public
contract and should be capability-driven, not set globally for convenience.

Policy:

- every public CMake target declares the lowest standard it genuinely needs with
  target-level compile features;
- optional feature targets may raise the consumer requirement only when the
  consumer links that feature;
- the default/umbrella surface should stay curated and should not pull
  experimental C++26-only features by accident;
- build probes should test capabilities such as reflection, `<simd>`,
  `<experimental/simd>`, `import std`, and platform support instead of assuming
  that a standard number implies availability;
- compatibility that costs nothing is welcome; compatibility that requires
  polyfills, duplicate implementations, public dialect aliases, or extra
  maintenance needs an explicit ergonomics/performance justification;
- the advertised support baseline is the lowest toolchain floor that the release
  evidence actually proves, not the lowest version that might parse the source
  files.

Expected target tiers before release:

| Target family | Baseline rule | Notes |
|---|---|---|
| core, JSON, HTTP, runtime, DB, crypto | C++23 unless the implementation genuinely needs more | This is the practical default user tier. |
| JSON reflection | C++26 plus a reflection-capable compiler probe | Keep isolated behind its own component/target. |
| standard SIMD fast paths | C++26 plus a `<simd>` compile probe | Scalar fallback must remain available. |
| vendor experimental SIMD fast paths | C++23 plus an `<experimental/simd>` compile probe | Treat as opportunistic, not a portable contract. |

A top-level developer preset may still request a newer language mode while the
project is being developed. That preset is not the public compatibility contract.
The exported/installable targets are authoritative for downstream consumers.

Conflux is intentionally not a broad portability project. It targets modern
Linux systems and the small set of compiler/CMake lanes that are continuously
exercised by the maintainer. Lower compiler, standard-library, CMake, or Ninja
versions may work if they happen to support the required module/header artifact
shape, but they are opportunistic compatibility and are not release blockers
unless they are promoted into the evidence matrix.

## Supported compiler matrix

Consumer presets are `core`, `json`, `http-api`, `web-server`, and `full`. They
use the default compiler selected by CMake, keep tests/examples/benchmarks and
FetchContent downloads off, and select only the requested stable feature set. A
consumer preset is supported for a release only when the compiler it resolves to
is one of that release's tested compiler families.

Developer and CI presets use named compilers and the full development feature
set because the module-first development lane uses `import std` and optional
reflection experiments. Public consumer requirements are still per-target, as
described in the language standard policy above, but public support is limited
to the concrete compiler versions captured in release evidence.

| Preset family | Expected compiler for the preview | Standard library | Current role |
|---|---|---|---|
| `debug-clang-libcxx`, `release-clang-libcxx`, `tsan-clang-libcxx` | Clang 21 through `clang++` | libc++ | primary Clang lane; ThinLTO release coverage |
| `debug-clang-stdcxx`, `release-clang-stdcxx`, `fuzz-clang-stdcxx` | Clang 21 through `clang++` | libstdc++ | parser/fuzz and mixed-stdlib coverage |
| `debug-gcc-stdcxx`, `release-gcc-stdcxx` | GCC 15 through `g++` | libstdc++ | GCC 15 lane; release LTO is intentionally disabled for this lane |
| `debug-gcc16-stdcxx`, `release-gcc16-stdcxx` | GCC 16 through `g++-16` | libstdc++ | GCC 16 / `import std` lane; GCC LTO coverage |
| `debug-p2996-gcc`, `release-p2996-gcc` | GCC 16 through `g++-16` with `-freflection` | libstdc++ | experimental JSON reflection lane |

A compiler is not considered supported merely because it accepts the language
mode. It must configure through one of the presets, build the relevant target
set, and pass the matching tests for that lane. For the preview, do not
advertise arbitrary GCC, Clang, AppleClang, MSVC, or distribution-default
compiler versions outside the evidence matrix.

Build and benchmark comparisons use each compiler's best working release
configuration. GCC 15 is the no-LTO release baseline until its release/LTO lane
is green. GCC 16 and Clang 21 are the LTO-capable release baselines. If one
source file or optional feature breaks a configuration, disable the conflicting
flag only for that compiler and source file/feature; do not downgrade the rest
of the target or library.

Benchmark policy:

- A DB row is exactly one benchmark function, parameter set, and mode/config.
- Normal rows target 0.4s to 2s measured time. Slow rows may exceed 2s; torture
  rows are adversarial. Slow and torture rows must still stay below 30s each.
- Suites are separate: smoke is tiny and CI-safe, normal uses 0.4s to 2s rows,
  slow is explicit >2s coverage, torture is adversarial.
- Suite runtime is budgeted too; many valid rows must not create an unbounded
  run.
- Calibrate first, warm up second, measure third: estimate the iteration count,
  run `ceil(iterations * 0.20)` warmup iterations, then measure that row once.
- Warmup is excluded from samples and recorded as metadata alongside measured
  iterations and measured seconds.
- Cold-path rows do not warm up unless explicitly marked, including cold parse,
  first route use, first TLS handshake, cold mmap/page-cache file reads, and
  allocator cold start.
- Metadata must record compiler, flags, CPU, kernel, governor/turbo state when
  known, target timing range, measured row time, iteration count, warmup count,
  and slow/torture classification.
- Allocation-sensitive rows must name whether arenas/pools reset per iteration
  or are intentionally reused.
- HTTP/io_uring row timing measures steady-state request handling, not server
  startup, unless the row name says startup or lifecycle.

## Supported kernel/runtime matrix

Conflux is Linux-only and `io_uring`-first. Runtime-facing feature sets require:

- a Linux kernel with `io_uring` enabled and usable by the current user or
  container;
- `liburing` headers and library discoverable through `pkg-config`;
- no seccomp/container policy that blocks ring setup or required opcodes.

Liburing-free component surfaces, including `core`, `json`, `file_io_sync`, and
`file_map`, may build without runtime support when selected through the matching
feature bundle or explicit component flags.

Runtime support is capability-driven rather than kernel-version-string-driven.
Startup probes and adaptive fallbacks decide which optional setup flags and
opcodes are active on the host. A feature is considered supported only when the
corresponding probe succeeds on that host.

Configuration must make fallback policy explicit. Use
`Config::feature_fallback` to choose fail-fast, warn-and-fallback, or
silent-fallback behavior; do not add hidden watcher/background negotiation for
core server startup. Diagnostic surfaces should report build info, capability
issues, and redacted effective config without printing secrets by default.

Required baseline behavior:

- `io_uring_queue_init*` succeeds;
- ordinary socket accept/connect/read/write operations work through the ring;
- timeout and cancel operations used by the socket layer are available or have a
  tested fallback;
- unsupported optional setup flags may be stripped during startup and must be
  visible in startup diagnostics.

Optional fast paths such as direct descriptors, registered buffers, SEND_ZC,
provided buffer rings, `NO_SQARRAY`, `CQE_MIXED`, `SQPOLL`, busy poll, and
future IOPOLL file rings are opportunistic. They must stay capability-gated and
must not become silent hard requirements. SEND_ZC throughput or threshold claims
require non-loopback evidence on ZC-capable NICs; loopback-only runs are
experimental correctness/sanity evidence.

## Release gates

`docs/release-checklist.md` is the operational checklist for collecting release
evidence. Before v1, every release candidate should at minimum have:

- full test pass on the primary Clang 21, GCC 15, and GCC 16 preset families
  available to the maintainer;
- ASan/UBSan or fuzz smoke coverage for parser-facing code;
- HTTP parser/security corpus run when HTTP code changed;
- benchmark comparison for claimed performance changes;
- updated docs for public API, migration, and security-impacting behavior;
- execution/concurrency-sensitive changes reviewed against
  `docs/concurrency-naming-model.md`.

## Test policy

Tests are behavioral contracts, not implementation details.

- Do not modify an existing test just to make a changed implementation pass.
- If a public symbol is renamed but the behavior is intentionally unchanged,
  update the affected test name or callsite to match the rename.
- If behavior changes in a broader way, add a new test for the new behavior and
  keep the old test around when it still describes the previous contract.
- Use distinct test names to avoid comparing different behaviors as if they
  were the same regression target.
- Prefer source fixes over test edits when a historical behavior used to work
  and still should work.

## Execution policy

Build artifacts, `ctest`, benchmarks, and examples should run outside the
sandbox so results match the real runtime environment.

- Use `./scripts/run-build-artifact.sh` for binaries and examples.
- Use `./scripts/run-ctest.sh` for test suites.
- Ask for wildcard approval once, then reuse it for the same script prefix.
- Keep runs representative; do not rely on sandboxed execution for final
  verification.

Temporary artifacts must be cleaned up by default.

- Scripts, examples, benchmarks, and tests should use unique paths under
  `${TMPDIR:-/tmp}` or `std::filesystem::temp_directory_path()`, not shared
  fixed names.
- Short-lived generated inputs, build trees, install prefixes, logs, TLS
  certificates, and keys should be removed with `trap` handlers or RAII guards
  on normal exit and failure paths.
- Explicit retention controls such as `KEEP_BUILD=1`, evidence output
  directories, benchmark artifact directories, and release staging directories
  may keep artifacts, but retaining files must be opt-in and documented by the
  script.
- Prefer in-memory credentials and test data when the API supports them. If an
  API currently accepts only filesystem paths, materialize unique files and
  unlink them as soon as the consumer has loaded them or when the owning scope
  exits.
