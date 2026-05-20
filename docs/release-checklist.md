# Pre-v1 release checklist

This checklist turns the policy gates in `docs/project-policy.md` into concrete
release-candidate evidence. It is intentionally stricter than a local branch
merge checklist and should be updated whenever a new public component, feature
bundle, or security-sensitive surface is added.

## Required evidence

| Gate | Evidence to attach before tagging | Notes |
|---|---|---|
| Compiler lanes | Preset name, compiler version, full configure/build/test log path | Cover the primary Clang lane and every GCC lane available to the maintainer. |
| Runtime preflight | Host kernel/container notes and `io_uring_queue_init*` success/failure | Capability probes matter more than kernel version strings. |
| Tests | `ctest` command, preset, result summary | Use `scripts/run-ctest.sh` where possible. |
| Sanitizers / fuzz | Sanitizer preset logs and parser-facing fuzz/security corpus result | Required when HTTP parser, JSON parser, URL/form decoding, or WebSocket framing changed. |
| Examples | `examples/compile` CTest result | Examples must compile, not necessarily run server loops in CI. |
| Header compatibility | Full `HEADER_INTERFACE` compile pass log | Pre-release only: enable examples, tests, and benchmarks to catch generated-header consumer regressions. Public header-mode profiles may stay examples-only. |
| Package/install | `build/package-config`, install prefix, package-smoke component list | Keep `docs/component-map.md` synchronized with installed components. |
| Docs/migration | List of public API docs touched or reason none changed | Required for public API, migration, config/default, or security-impacting changes. |
| Cost/lifetime docs | Confirmation that `docs/cost-lifetime-model.md` still matches changed HTTP, JSON, file, or runtime behavior | Required when ownership, copying, allocation, blocking, or zero-copy behavior changes. |
| Benchmarks | Same-machine benchmark artifact path and comparison summary | Required for claimed performance changes; use perf presets only. |
| Security review | Affected component and corpus/regression tests | Required for auth/session/password/token, parser, path traversal, proxy, TLS, DB, DNS, and process-spawn surfaces. |
| Alias cleanup | Remaining aliases or confirmation none remain | Alias removal belongs to the final release-cleanup branch only. |

## Prerelease command lanes

Header-interface build and install:

```sh
python3 scripts/check_no_std_streams.py
cmake -S . -B /tmp/conflux-header -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCONFLUX_USE_MOCK_LIBURING=ON \
  -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
  -DCONFLUX_BUILD_TESTS=OFF \
  -DCONFLUX_BUILD_BENCHMARKS=OFF \
  -DCONFLUX_ENABLE_DB=OFF
cmake --build /tmp/conflux-header
cmake --install /tmp/conflux-header --prefix /tmp/conflux-install
```

Installed package smoke from outside the source tree:

```sh
cmake -S cmake/package-smoke -B /tmp/conflux-smoke -G Ninja \
  -DCMAKE_CXX_COMPILER=g++ \
  -Dconflux_DIR=/tmp/conflux-install/lib/cmake/conflux \
  -DCONFLUX_PACKAGE_SMOKE_INTERFACE_MODE=HEADER_INTERFACE \
  -DCONFLUX_PACKAGE_SMOKE_COMPONENTS="core;json;http"
cmake --build /tmp/conflux-smoke
ctest --test-dir /tmp/conflux-smoke --output-on-failure
```

Optional DB-enabled lane, only on hosts with libpq headers:

```sh
cmake -S . -B /tmp/conflux-db -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCONFLUX_USE_MOCK_LIBURING=ON \
  -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
  -DCONFLUX_BUILD_TESTS=OFF \
  -DCONFLUX_BUILD_BENCHMARKS=OFF \
  -DCONFLUX_ENABLE_DB=ON
cmake --build /tmp/conflux-db
```

Test lane, when Catch2 is available from the system or an approved cache:

```sh
cmake -S . -B /tmp/conflux-tests -G Ninja \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCONFLUX_USE_MOCK_LIBURING=ON \
  -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
  -DCONFLUX_BUILD_TESTS=ON \
  -DCONFLUX_TEST_CATCH2_PROVIDER=SYSTEM \
  -DCONFLUX_BUILD_BENCHMARKS=OFF
cmake --build /tmp/conflux-tests
ctest --test-dir /tmp/conflux-tests --output-on-failure
```

## Component-specific checks

### Build/package

- `scripts/check-package-config.sh` passes.
- Installed `find_package(conflux REQUIRED COMPONENTS ...)` works for the
  components listed in `docs/component-map.md`.
- Install/package smokes cover the selected public interface mode. Run module
  and header package lanes separately; mixed import/include consumers are not a
  supported release gate.
- Feature bundles in `cmake/ConfluxPresets.cmake` match the bundle descriptions
  in `docs/component-map.md`.

### Runtime / io_uring / worker

- Runtime docs still state that `io_uring` is a hard requirement for runtime,
  HTTP server/client components, async file I/O, socket I/O, and tests or
  benchmarks that exercise those surfaces.
- New optional fast paths are capability-gated and have a fallback or clear
  startup diagnostic.
- Blocking wait or raw syscall-style surfaces use `blocking_*`; executor-owned
  synchronous surfaces use `sync_*`; coroutine surfaces use `async_*`.

### HTTP

- `docs/http-server-api.md` matches handler execution placement: handlers run on
  ring threads unless code explicitly moves work elsewhere.
- HTTP parser/security corpus covers any request parsing, header, chunking,
  Host, Content-Length, Transfer-Encoding, range/static, or proxy change.
- `docs/cost-lifetime-model.md` matches request view/owned request lifetimes,
  response body ownership, static/file zero-copy caveats, and TLS behavior.
- Lifecycle/backpressure docs match `DrainOptions`, `DrainReport`,
  `OverflowPolicy`, SSE overflow mapping, and `HttpPressureMetrics`.
- HTTP/1 rejection taxonomy docs match status codes and passive server metrics:
  `malformed_content_length` -> 400 -> `rejections.malformed_content_length`,
  `duplicate_content_length` -> 400 -> `rejections.duplicate_content_length`,
  `content_length_with_transfer_encoding` -> 400 ->
  `rejections.content_length_with_transfer_encoding`,
  `header_block_too_large` -> 431 -> `rejections.header_block_too_large`, and
  `body_too_large` -> 413 -> `rejections.body_too_large`.
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
- Reflection docs state GCC/P2996 requirements when reflection APIs are enabled.

### DB, files, process, DNS, SMTP

- File-layer docs preserve the boundary between POSIX sync file helpers, mmap
  helpers, and async/runtime-backed file I/O.
- DB docs match libpq feature-gate behavior and pool/query contracts.
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

- compatibility alias cleanup is complete, or remaining aliases are explicitly
  internal and not advertised;
- public examples use final preview names only;
- README can onboard a new user without sending them to TODO files;
- release notes exist under `docs/releases/`;
- Apache-2.0 license metadata, `SECURITY.md`, and contribution expectations are
  present;
- package components match the advertised preview scope in
  `docs/component-map.md`;
- proof/evidence artifacts are attached separately for the advertised scope.
