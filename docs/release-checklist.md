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
| Benchmarks | Same-machine benchmark artifact path and comparison summary | Required for claimed performance changes; use perf presets only. |
| Security review | Affected component and corpus/regression tests | Required for auth/session/password/token, parser, path traversal, proxy, TLS, DB, DNS, and process-spawn surfaces. |
| Alias cleanup | Remaining aliases or confirmation none remain | Alias removal belongs to the final release-cleanup branch only. |

## Component-specific checks

### Build/package

- `scripts/check-package-config.sh` passes.
- Installed `find_package(conflux REQUIRED COMPONENTS ...)` works for the
  components listed in `docs/component-map.md`.
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
- Auth, CSRF, JWT, cookie, rate-limit, and password-hash docs are updated when
  defaults or policy knobs change.

### JSON

- Parser/DOM ownership docs match the code: borrowed parse requires stable input;
  owning parse is explicit.
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
