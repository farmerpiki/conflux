# Pre-v1 release checklist

This checklist turns the policy gates in `docs/project-policy.md` into concrete
release-candidate evidence. It is intentionally stricter than a local branch
merge checklist and should be updated whenever a new public component, feature
bundle, or security-sensitive surface is added.

## Required evidence

| Gate | Evidence to attach before tagging | Notes |
|---|---|---|
| Compiler lanes | Preset name, compiler version, CMake/Ninja version, full configure/build/test log path | Cover Clang 21, GCC 15, and GCC 16 unless the release notes explicitly narrow the preview. |
| Runtime preflight | Host kernel/container notes and `io_uring_queue_init*` success/failure | Capability probes matter more than kernel version strings. |
| Tests | `ctest` command, preset, result summary | Use `scripts/run-ctest.sh` where possible. |
| Sanitizers / fuzz | Sanitizer preset logs and parser-facing fuzz/security corpus result | Required when HTTP parser, JSON parser, URL/form decoding, multipart parsing, or WebSocket framing changed. |
| Examples | `examples/compile` CTest result | Examples must compile, not necessarily run server loops in CI. |
| Module package | Full `MODULE_INTERFACE` configure/build/test/package log | Primary prerelease consumption mode; use checked presets and fail clearly on unsupported toolchains. |
| Generated header artifact | Staged artifact tree and `HEADER_INTERFACE` compile pass log | Generated headers are release artifacts from module sources, not hand-maintained source. |
| Package/install | `build/package-config`, install prefix, package-smoke component list | Keep `docs/component-map.md` synchronized with installed components. |
| Docs/migration | List of public API docs touched or reason none changed | Required for public API, migration, config/default, or security-impacting changes. |
| Cost/lifetime docs | Confirmation that `docs/cost-lifetime-model.md` still matches changed HTTP, JSON, file, or runtime behavior | Required when ownership, copying, allocation, blocking, or zero-copy behavior changes. |
| Build-cost / size baseline | `scripts/compile_time_bench.py` JSON output and `scripts/measure-build-costs.py --json` output for the selected SKU build | Record-only for first preview; do not treat as a performance claim or pass/fail budget. |
| Benchmarks | Same-machine benchmark artifact path and raw-run summary | Required only for claimed performance changes; use perf presets only. Final public performance proof is out of scope for the prerelease documentation cleanup. |
| Security review | Affected component and corpus/regression tests | Required for auth/session/password/token, parser, path traversal, proxy, TLS, DB, DNS, and process-spawn surfaces. |
| Alias cleanup | Remaining aliases or confirmation none remain | Confirm no deprecated public compatibility aliases are advertised for the preview surface. |

## Prerelease command lanes

Before tagging, re-check whether GCC debug sanitizer coverage or GCC 15 release
LTO coverage can be narrowed back on. Prefer the smallest workaround if GCC
still has module/sanitizer/LTO trouble: disable only the problematic sanitizer
or LTO flag, and only for the affected translation unit if CMake can express
that cleanly. If ongoing module splitting removes the failure, keep the normal
lane instead of carrying a special case.

Module-interface build and install:

```sh
python3 scripts/check_no_std_streams.py
python3 scripts/check-first-contact-public-dialect.py
python3 scripts/check-planning-state.py
python3 scripts/check-release-docs.py
python3 scripts/check-release-skus.py
python3 scripts/check-component-map.py
python3 scripts/check-api-surface-map.py
python3 scripts/check-package-docs.py
python3 scripts/check-release-notes.py
cmake --preset release-clang-libcxx
cmake --build --preset release-clang-libcxx
ctest --preset release-clang-libcxx --output-on-failure
scripts/run-install-tree-smoke.sh \
  --interface-mode MODULE_INTERFACE \
  --feature-set release-http-api \
  --components 'http;json' \
  --forbid-components "$(python3 scripts/package-smoke-forbidden-components.py http)" \
  --forbid-external-deps "$(python3 scripts/external-dependency-tokens.py . --policy http)"
python3 scripts/compile_time_bench.py \
  --build /tmp/gcc-16/compile-time-bench \
  --feature-set release-http-api \
  --interface-mode MODULE_INTERFACE \
  --pretty
cmake -S . -B /tmp/gcc-16/release-http-api-cost-baseline -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/lib/llvm/21/bin/clang++ \
  -DCMAKE_C_COMPILER=/usr/lib/llvm/21/bin/clang \
  -DCONFLUX_FEATURE_SET=release-http-api \
  -DCONFLUX_INTERFACE_MODE=MODULE_INTERFACE \
  -DCONFLUX_ENABLE_LTO=ON \
  -DCONFLUX_LTO_MODE=THIN \
  -DCONFLUX_BUILD_TESTS=OFF \
  -DCONFLUX_BUILD_EXAMPLES=ON \
  -DCONFLUX_BUILD_BENCHMARKS=ON
cmake --build /tmp/gcc-16/release-http-api-cost-baseline
python3 scripts/measure-build-costs.py \
  /tmp/gcc-16/release-http-api-cost-baseline \
  --sku release-http-api \
  --json
```

Generated-header artifact build and install:

```sh
cmake -S . -B /tmp/conflux-header -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
  -DCONFLUX_BUILD_TESTS=OFF \
  -DCONFLUX_BUILD_BENCHMARKS=OFF \
  -DCONFLUX_POSTGRES_PROVIDER=OFF
cmake --build /tmp/conflux-header
cmake --install /tmp/conflux-header --prefix /tmp/conflux-install
find /tmp/conflux-install/include/conflux -maxdepth 2 \( -name 'pg*' -o -name 'db.hxx' \) -print
```

The `find` command should print no DB headers when `CONFLUX_POSTGRES_PROVIDER=OFF`.
The generated-header lane stages release artifacts and compile evidence; it is
not the primary source-consumption contract.

Release artifact staging:

```sh
scripts/stage-release-artifacts.sh \
  --stage-dir /tmp/conflux-release-artifacts/stage \
  --no-tarball
```

Installed liburing-free package smoke from outside the source tree. This is the
required generated-header artifact lane for dependency-light installs and does
not publish runtime-facing/http package components.

```sh
scripts/check-package-smoke-liburing-free.sh
```

Standalone JSON package smoke from outside the source tree. This runs the
`release-json` install smoke in header mode and module mode, including mixed
module/header and public module import downstream consumers, while asserting
that HTTP/runtime/DB/TLS/compression components and unrelated external
dependencies are not exposed.

```sh
scripts/check-package-smoke-json-standalone.sh
```

Runtime package smoke, only on hosts with real liburing discoverable through
`pkg-config` and a non-mock install:

```sh
scripts/check-package-smoke-runtime.sh
```

Optional DB-enabled build lane, only on hosts with libpq headers:

```sh
cmake -S . -B /tmp/conflux-db -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
  -DCONFLUX_BUILD_TESTS=OFF \
  -DCONFLUX_BUILD_BENCHMARKS=OFF \
  -DCONFLUX_POSTGRES_PROVIDER=LIBPQ
cmake --build /tmp/conflux-db
```

Optional DB integration/pressure lane, only on hosts with libpq, Catch2, a live
PostgreSQL database, and io_uring support:

```sh
cmake -S . -B /tmp/conflux-db-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
  -DCONFLUX_BUILD_TESTS=ON \
  -DCONFLUX_BUILD_BENCHMARKS=OFF \
  -DCONFLUX_POSTGRES_PROVIDER=LIBPQ \
  -DCONFLUX_PG_TEST_CONNINFO="postgresql:///conflux_test"
cmake --build /tmp/conflux-db-tests --target conflux_db_integration
ctest --test-dir /tmp/conflux-db-tests --output-on-failure -L 'db|integration'
```

Test lane, when Catch2 is available from the system or an approved cache:

```sh
cmake -S . -B /tmp/conflux-tests -G Ninja \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
  -DCONFLUX_BUILD_TESTS=ON \
  -DCONFLUX_TEST_CATCH2_PROVIDER=SYSTEM \
  -DCONFLUX_BUILD_BENCHMARKS=OFF
cmake --build /tmp/conflux-tests
ctest --test-dir /tmp/conflux-tests --output-on-failure
```

Optional third-party protocol conformance lane, when `h2spec` or Autobahn
`wstest` are available from the system or an approved cache:

```sh
cmake -S . -B /tmp/conflux-third-party -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCONFLUX_FEATURE_SET=release-full \
  -DCONFLUX_BUILD_TESTS=ON \
  -DCONFLUX_BUILD_EXAMPLES=OFF \
  -DCONFLUX_ENABLE_THIRD_PARTY_TESTS=ON
cmake --build /tmp/conflux-third-party --target conflux_third_party_conformance_server
ctest --test-dir /tmp/conflux-third-party -L third-party --output-on-failure
```

## Benchmark Artifact Timing

Do not publish final runtime proof or benchmark graphs as part of this source
tree cleanup. If a release note makes a performance claim later, collect the
same-machine benchmark artifacts after:

- public formatting and human-readable cleanup are complete;
- public examples and first-contact docs use final preview spelling;
- advertised component/package/interface modes are settled;
- the advertised compiler and CMake baseline matches the actual release-evidenced
  floor rather than an aspirational or untested compatibility claim;
- benchmark cases, graph scripts, and comparison targets are settled;
- release notes contain placeholders for benchmark artifact paths.

Keep this source tree to source, small manifests, and concise benchmark
summaries. Bulky logs, raw benchmark rows, perf data, and generated graphs stay
outside the tracked source tree.

## Component-specific checks

### Build/package

- `scripts/check-package-config.sh` passes.
- `scripts/stage-release-artifacts.sh` stages source modules, installed package
  config, generated headers, bridge manifest metadata, and the release evidence
  template.
- Planning/release docs guards pass:
  `scripts/check-planning-state.py`, `scripts/check-release-docs.py`,
  `scripts/check-release-skus.py`, `scripts/check-component-map.py`,
  `scripts/check-api-surface-map.py`, `scripts/check-package-docs.py`, and
  `scripts/check-release-notes.py`.
- Liburing-free HEADER_INTERFACE installs are generated-header artifact
  evidence. They must request `core;types;json;file_io_sync` availability, with
  the smoke compile lane using `core;json;file_io_sync`, not runtime-facing/http components.
- `CONFLUX_API_SURFACE=curated|extended|complete` smoke behavior is validated by the
  selected aggregate lane; core/json-only builds use explicit leaf imports because
  aggregate re-exports are feature-lane dependent and not used to gate component
  visibility.
- Header-interface release artifacts always include generated implementation
  sources for implementation-backed public components.
- `scripts/check-package-smoke-runtime.sh` passes or skips explicitly based on
  real `liburing` availability. It is the lane that requests
  `core;json;http;file_io_sync;work`.
- Installed `find_package(conflux REQUIRED COMPONENTS http json)` works in
  module and generated-header package lanes; the package config imports the
  required dependency closure.
- Install/package smokes cover the selected public interface mode. Run the
  module package lane as primary and the generated-header artifact lane
  separately; mixed import/include consumers are not a supported release gate.
- Mixed import/include package smoke is an internal drift and ODR guard, not a
  supported external consumption mode.
- Feature bundles in `cmake/ConfluxPresets.cmake` match the bundle descriptions
  in `docs/component-map.md`.

### Runtime / io_uring / worker

- Runtime docs still state that `io_uring` is a hard requirement for runtime,
  HTTP server/client components, async file I/O, socket I/O, and tests or
  benchmarks that exercise those surfaces.
- `docs/execution-model.md` cancellation semantics match async task, socket,
  TLS, `send_zc`, async file, DNS, and DB behavior when those surfaces change.
- New optional fast paths are capability-gated and have a fallback or clear
  startup diagnostic.
- Blocking wait or raw syscall-style surfaces use `blocking_*`; executor-owned
  synchronous surfaces use `sync_*`; coroutine surfaces use `async_*`.

### HTTP

- `docs/http-server-api.md` matches handler execution placement: handlers run on
  ring threads unless code explicitly moves work elsewhere.
- HTTP parser/security corpus covers any request parsing, header, chunking,
  Host, Content-Length, Transfer-Encoding, range/static, or proxy change.
- Multipart parser changes run `fuzz_multipart_smoke`; the seed corpus covers
  filename/content-type parameters, malformed boundaries, many tiny parts, and
  oversized part headers.
- Cookie/header/security policy changes run `fuzz_cookie_header_smoke`,
  `fuzz_cookie_signing_smoke`, and `fuzz_security_policy_smoke`; the seed
  corpus covers cookie OWS/duplicates, CORS preflight, CSRF double-submit,
  forwarded/trusted proxy headers, cache-control, ETag, and security headers.
- Static-file path handling changes run `fuzz_static_path_smoke`; the seed corpus
  covers encoded traversal, UTF-8 oddities, repeated separators, symlink
  attempts, and absolute-path attempts against both normalization and contained
  file opens.
- `docs/cost-lifetime-model.md` matches request view/owned request lifetimes,
  response body ownership, static/file zero-copy caveats, and TLS behavior.
- Lifecycle/backpressure docs match `DrainOptions`, `DrainReport`,
  `OverflowPolicy`, SSE overflow mapping, and `HttpPressureMetrics`.
- HTTP/1 rejection taxonomy docs match status codes and passive server metrics:
  `request_line_too_large` -> 414 -> `rejections.request_line_too_large`,
  `malformed_content_length` -> 400 -> `rejections.malformed_content_length`,
  `duplicate_content_length` -> 400 -> `rejections.duplicate_content_length`,
  `content_length_with_transfer_encoding` -> 400 ->
  `rejections.content_length_with_transfer_encoding`,
  `header_block_too_large` -> 431 -> `rejections.header_block_too_large`,
  `too_many_headers` -> 431 -> `rejections.too_many_headers`,
  `invalid_chunk` -> 400 -> `rejections.invalid_chunk`,
  `body_too_large` -> 413 -> `rejections.body_too_large`,
  `header_timeout` -> 408 -> `rejections.header_timeout`, and
  `body_timeout` -> 408 -> `rejections.body_timeout`.
- Auth, CSRF, JWT, cookie, rate-limit, and password-hash docs are updated when
  defaults or policy knobs change.

### JSON

- Parser/DOM ownership docs match the code: borrowed parse requires stable input;
  owning parse is explicit.
- `docs/cost-lifetime-model.md` matches JSON borrowed/owned parse, decode, and
  writer allocation behavior without making the native provider the framework
  contract.
- HTTP/app framework code depends on `conflux.json.boundary` provider contracts,
  not directly on a concrete JSON provider unless the module is a native-provider
  convenience edge.
- Reflection docs state the GCC 16/P2996 requirements when reflection APIs are enabled.

### DB, files, process, DNS, SMTP

- File-layer docs preserve the boundary between POSIX sync file helpers, mmap
  helpers, and async/runtime-backed file I/O.
- DB docs match libpq feature-gate behavior and pool/query/pressure contracts.
- DB-off header-interface installs do not expose generated `conflux/pg*` or
  `conflux/db.hxx` headers.
- Process-spawn changes include argument/env/lifetime/error-path tests.
- DNS and SMTP changes include timeout/cancel/error-path tests.

## Release notes minimum

Every release candidate note should include:

- supported compiler/preset matrix used for that candidate;
- kernel/runtime capability notes;
- enabled optional protocol/storage features;
- migration notes for renamed or removed public symbols;
- security-impacting changes and mitigations;
- benchmark artifact references for any performance claim.

## Public preview cleanup

Before tagging the first public preview:

- deprecated public compatibility alias cleanup is complete, or remaining aliases
  are explicitly internal and not advertised;
- public examples use final preview names only;
- README can onboard a new user without sending them to TODO files;
- release notes exist under `docs/releases/`;
- Apache-2.0 license metadata, `SECURITY.md`, and contribution expectations are
  present;
- package components match the advertised preview scope in
  `docs/component-map.md`;
- proof/evidence artifacts are attached separately for the advertised scope.
