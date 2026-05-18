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

## Supported compiler matrix

The project targets C++26 and uses CMake presets as the source of truth for
supported developer configurations.

| Preset family | Compiler | Standard library | Current role |
|---|---|---|---|
| `debug-clang-libcxx`, `release-clang-libcxx`, `tsan-clang-libcxx` | `clang++` | libc++ | primary Clang lane |
| `debug-clang-stdcxx`, `release-clang-stdcxx`, `fuzz-clang-stdcxx` | `clang++` | libstdc++ | parser/fuzz and portability lane |
| `debug-gcc-stdcxx`, `release-gcc-stdcxx` | `g++` | libstdc++ | GCC 15-compatible lane; sanitizers are disabled in the preset because of known GCC 15 issues |
| `debug-gcc16-stdcxx`, `release-gcc16-stdcxx` | `g++-16` | libstdc++ | GCC 16 / `import std` lane |
| `debug-p2996-gcc` | `g++-16` with `-freflection` | libstdc++ | experimental JSON reflection lane |

A compiler is not considered supported merely because it accepts the language
mode. It must configure through one of the presets, build the relevant target
set, and pass the matching tests for that lane.

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

- full test pass on the primary Clang and GCC preset families available to the
  maintainer;
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
