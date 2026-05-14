# Parallel Implementation Priority Plan

Date: 2026-05-14

This document converts the current code/doc state into branchable work lanes. It is
intentionally not a single linear backlog: the project is still pre-release, but
the remaining work now spans mostly independent components. Use this file to pick
parallel branches without repeatedly re-deciding global priority.

## Reading order

1. Keep `proposals/perf_ideas.md` as the current io_uring/perf inventory.
2. Keep `/home/claudiu/conflux_dev/api_traps.md` as the migration-trap
   reference, but apply the project-specific clarifications in this file first
   where they differ.
3. Use this file for branch selection and merge sequencing.
4. Do not start alias elimination until the release-cleanup lane says so.

## Current state assessment

### What is already far enough along

- io_uring quick wins mostly landed: multishot accept/recv, provided buffer rings,
  registered file buffers, registered send buffers, SEND_ZC for the main HTTP send
  path, CQE batch drains, feature probing, TCP_NODELAY/TCP_QUICKACK, busy-poll
  config, ring/io-wq affinity, hugepage hints, DONTFORK hints, and MPMC global
  injection.
- App-level blocking HTTP bridge was removed from `app_http`; remaining publication
  compatibility is still a named sync publication seam.
- Worker task bodies were moved substantially toward awaitable helpers. The full
  pipeline path is mostly async, but the runtime still has a compatibility wait
  seam that needs to be retired or isolated.
- Module CMI fragility has a proven mitigation: thin exported interfaces plus
  coroutine-heavy implementation units, especially around cancellation/socket/coro
  surfaces.
- API naming policy is now clarified:
  - `blocking_`: raw/blocking syscall-style helpers such as wrappers around
    `::write(fd, ...)`.
  - `sync_`: executor/task APIs that run on the task/executor model but expose a
    non-coroutine success/failure surface.
  - `async_`: coroutine APIs.

### Main gaps still worth implementing

- Worker runtime: retire or isolate the remaining compatibility wait bridge; finish
  background ingestion runtime convergence; then move the remaining compatibility
  surface behind explicitly named `sync_`/`blocking_` APIs.
- Security: replace the current password-hash mechanism before widening API work.
- JSON boundary: isolate app/framework JSON usage behind traits/adapters before
  designing a new parser/DOM. The parser work is important, but the boundary cleanup is
  the prerequisite that keeps it branchable.
- HTTP/io_uring: finish SEND_ZC edge cases and measurement; add storage-ring IOPOLL;
  defer RECV_ZC implementation until kernel support is stable, but prepare the recv
  abstraction so the later branch is narrow.
- Perf/CI: make the benchmark/profiling harness first-class before accepting further
  low-level perf claims.
- Docs/examples: update concurrency, naming, and handler-execution docs so coding
  agents stop reintroducing the old hidden-offload/sync-handler model.

### Guidance conflicts to resolve explicitly

- `api_traps.md` says blocking handler work should either be
  impossible or be auto-offloaded. For conflux, do **not** normalize arbitrary sync handlers by
  secretly offloading them away from ring threads. HTTP server handlers run on
  io_uring/ring threads. Blocking syscall wrappers are named `blocking_*`; sync
  executor APIs are named `sync_*`; coroutine APIs are named `async_*`.
- `api_traps.md` says JSON should be pluggable middleware, not core. Keep that as
  the destination, but do not implement a large JSON library before route/app JSON
  boundaries are isolated.
- Older perf inventories marked network send buffers, SEND_ZC, affinity, and hot
  struct padding as missing or partial. Treat the newest inventory as authoritative:
  those are now done or mostly done, with only edge cases/measurement left.
- Alias elimination is deliberately **lowest priority**. Aliases stay until the last
  release-cleanup pass so branch work does not churn large API surfaces while major
  internals are still moving.

## Branching model

### Branch naming

Use one component prefix per branch:

| Prefix | Component |
|---|---|
| `worker/` | worker runtime, background task runtime, task frame allocation |
| `http/` | HTTP server, routing, request/response API |
| `uring/` | low-level io_uring, socket/file I/O primitives |
| `json/` | JSON adapter boundary, JSON serde, parser/DOM experiments |
| `auth/` | password hashing, session/token auth, secret handling |
| `build/` | CMake, CI, benchmark harness, packaging |
| `docs/` | design docs, examples, migration notes |
| `release/` | final cleanup only, including alias removal |

### Merge rules

- Prefer narrow branches that touch one component tree plus tests/docs.
- Do not mix broad renames with semantic changes.
- Do not remove aliases in feature branches.
- If a branch needs an API rename for clarity, add the new name, update local call
  sites, keep old aliases, and add a release-cleanup note.
- If a branch changes executor/task semantics, it must merge before branches that
  depend on those semantics.
- If a branch changes JSON traits/adapters, route/app JSON cleanup branches must
  rebase onto it rather than inventing local adapter shapes.

## Cross-component dependency graph

```text
docs/concurrency-naming-model
  -> worker/no-wait-bridge
      -> worker/background-ingestion-runtime
      -> worker/taskpromise-frame-pool
      -> http/sync-async-handler-docs

json/boundary-traits
  -> json/route-response-writer
  -> json/app-boundary-cleanup
  -> json/parser-dom-prototype

auth/password-hash-replacement
  -> auth/secret-config-cleanup
  -> auth/session-token-audit

build/perf-harness-stabilize
  -> uring/sendzc-edge-measurement
  -> uring/iopoll-storage-ring
  -> worker/queue-contention-profile
  -> http/ring-layout-c2c-verify

release/alias-removal
  -> only after all pre-release blockers are merged
```

## Component priority lists

Priorities:

- **P0**: unblocker or correctness/security risk; can start now.
- **P1**: next practical implementation branch after P0 dependency merge.
- **P2**: needs measurement/prototype/design first.
- **P3**: defer until toolchain/kernel/API maturity improves.
- **R**: final release cleanup only.

### Worker/runtime/executor lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P0 | `worker/no-wait-bridge` | Retire or isolate the remaining compatibility wait bridge; leave at most one explicitly named compatibility seam. | Conflicts with worker task dispatch only; avoid JSON/auth changes. | No direct wait bridge calls outside the dedicated adapter; async worker task bodies remain awaitable; tests pass. |
| P0 | `docs/concurrency-naming-model` | Document executor-only task model and `blocking_`/`sync_`/`async_` names. | Docs-only; can merge immediately. | Docs state HTTP handlers run on ring threads and no hidden offload normalization exists. |
| P1 | `worker/background-ingestion-runtime` | Merge/migrate background ingestion runtime surface onto the worker runtime model. | Depends on `worker/no-wait-bridge`; should not touch auth/json. | Background ingestion uses the same runtime conventions as other worker tasks. |
| P1 | `worker/taskpromise-frame-pool` | Extend coroutine frame pool coverage from `EagerChain` to `TaskPromise<T>` where safe. | May touch `work/root.cxx`; avoid overlapping with no-wait branch unless sequenced. | Hot request-path `Task<void>` no longer uses global `::operator new` in steady-state when the pool option is enabled; sanitizer behavior remains safe. |
| P2 | `worker/queue-contention-profile` | Profile local deque locks, steal path, `admission_mtx_`, and seq_cst fence pair under HTTP load. | Depends on `build/perf-harness-stabilize`; profiling branch can be independent. | Output profile notes and either a minimal lock-removal patch or a documented no-change decision. |
| P3 | `worker/p2300-prototype` | Prototype P2300/io_uring scheduler behind an experimental target. | Do not mix with active V2 runtime migration. | Prototype compiles separately; no public API commitment. |

Recommended next worker branch: `worker/no-wait-bridge`.

### HTTP server / routing / handler API lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P0 | `http/handler-execution-docs` | Update docs/examples to match ring-thread execution and naming policy. | Docs/examples only; can merge with `docs/concurrency-naming-model`. | No docs imply arbitrary sync handlers are offloaded automatically. |
| P1 | `http/sendzc-mapped-edge` | Finish mapped-file header+body SEND_ZC edge case or document why fallback remains. | Touches HTTP send path; avoid concurrent send-buffer refactors. | Mapped-file send path has explicit measured behavior and fallback rationale. |
| P1 | `http/send-threshold-bench` | Validate/adapt SEND_ZC thresholds under realistic HTTP load. | Depends on perf harness. | Threshold defaults are backed by benchmark notes; counters remain exposed. |
| P1 | `http/limits-defaults` | Audit header/body/timeout defaults against hardened-by-default guidance. | Mostly HTTP config/parser; avoid route API renames. | Safe defaults documented and tested. |
| P2 | `http/ring-layout-c2c-verify` | Verify `Ring` hot/cold field grouping with `perf c2c`; pad only if measured. | Depends on perf harness; low conflict. | Either measured padding patch or no-change note. |
| P2 | `http/examples-route-minimal` | Add/keep a minimal route/JSON response example that stays under the target ceremony budget. | Examples/docs; can run parallel after JSON boundary shape is stable. | Example compiles in CI. |
| P3 | `http2/core-prototype` | HTTP/2 core exploration. | Start only after handler/runtime model stops moving. | Separate target or feature flag; no core churn. |

Recommended next HTTP branch: `http/handler-execution-docs`, then `http/sendzc-mapped-edge`.

### Low-level io_uring / socket / file I/O lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P0 | `uring/setup-flag-fallback-log` | Log requested-vs-active setup flags after EINVAL stripping. | Low conflict; touches ring init/logging. | Startup log makes `NO_SQARRAY`, `SUBMIT_ALL`, `CQE_MIXED`, etc. requested/active/stripped status visible. |
| P1 | `uring/iopoll-storage-ring` | Add storage-only `IORING_SETUP_IOPOLL` support for O_DIRECT/NVMe file rings. | Touches file I/O/ring init; independent of HTTP send path. | IOPOLL cannot mix with sockets; config makes storage-only scope explicit; tests cover fallback. |
| P1 | `uring/sendzc-edge-measurement` | Add focused benchmark/counters around SEND_ZC fallback paths. | Depends on perf harness; independent of auth/json. | Bench output can decide mapped-file/TLS fallback policy. |
| P2 | `uring/recv-abstraction-for-zc` | Refactor recv buffer ownership so RECV_ZC can slot in later. | Can run before kernel support, but avoid changing behavior. | Existing recv behavior unchanged; abstraction names lifetime/pinning requirements. |
| P3 | `uring/recv-zc` | Implement `IORING_OP_RECV_ZC`. | Wait for stable target kernel support and abstraction branch. | Feature-gated, runtime-probed, clear fallback. |

Recommended next uring branch: `uring/setup-flag-fallback-log` or `uring/iopoll-storage-ring`.

### JSON / serde / app boundary lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P0 | `json/boundary-traits` | Introduce a thin JSON provider/serde trait boundary without changing parser implementation. | Touches JSON-facing app/route modules; should merge before route JSON cleanup. | App/framework code stops directly depending on one concrete JSON representation at major boundaries. |
| P1 | `json/route-response-writer` | Route JSON responses through writer/adaptor APIs that can write to buffers/streams without forced owning DOM materialization. | Depends on `json/boundary-traits`; avoid parser internals. | Response path can serialize through adapter; allocation points are explicit. |
| P1 | `json/app-boundary-cleanup` | Clean remaining app-level JSON usage behind the trait. | Depends on boundary traits; can split by route group. | No new JSON provider lock-in in app code. |
| P2 | `json/parser-dom-design` | Produce design/prototype for view-first parser + arena-backed DOM + reflection serde. | Do not implement broad parser while app boundaries are still concrete. | Prototype/design names memory model, error model, UTF/number policy, and integration API. |
| P2 | `json/bench-fixtures` | Add JSON perf/correctness fixtures for route payloads and malformed inputs. | Can run parallel with design; low conflict. | Benchmarks include strict UTF-8, large numbers, missing/out-of-order keys, duplicate keys, deep nesting. |
| P3 | `json/reflection-serde` | C++26 reflection or PFR bridge under the stable trait shape. | Depends on trait and parser/DOM decision. | No macros, no hard JSON provider dependency, clear compile-time cost measurement. |
| P3 | `json/schema-pointer-patch` | JSON Pointer/Patch/schema support. | After core boundary/parser shape. | Feature targets separate from core hot path. |

Recommended next JSON branch: `json/boundary-traits`.

### Auth / security lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P0 | `auth/password-hash-replacement` | Replace current password hashing with a dedicated password-hash implementation/wrapper. Prefer a proven Argon2id/libsodium-style dependency if the current code has no equivalent primitive. | Independent of worker/JSON unless login routes are touched; coordinate DB migration. | New hashes include algorithm/version/params; existing users have a migration/rehash path; tests cover verify/fail/upgrade. |
| P1 | `auth/secret-config-cleanup` | Move auth secrets/pepper/session config into typed config with explicit missing-config errors. | Depends on password wrapper shape; mostly config/auth. | No silent default production secrets. |
| P1 | `auth/session-token-audit` | Audit session/token creation, expiry, storage, revocation, and error surfaces. | Can run after password branch; avoid route ergonomics changes. | Threat model notes and tests for expiry/revocation. |
| P2 | `auth/rate-limit-hooks` | Add hooks for login/API abuse controls without baking policy into core. | Can run parallel with HTTP limits if interfaces are stable. | Hook points exist; default remains safe/simple. |

Recommended next auth branch: `auth/password-hash-replacement`.

### Build / tests / perf / CI lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P0 | `build/perf-harness-stabilize` | Make perf harness reproducible: benchmark presets, symbols, fixed inputs, result artifact path, and docs. | Independent; unlocks several perf branches. | HTTP/file/worker benchmark commands are documented and runnable. |
| P0 | `build/module-fragility-regression` | Add/keep regression docs/tests for thin-interface module pattern around coroutine-heavy modules. | Build/docs; low conflict. | Agents stop reintroducing heavy coroutine bodies into fragile module interfaces. |
| P1 | `build/ci-sanitizer-perf-split` | Separate sanitizer correctness lanes from release/perf lanes. | CMake/CI only. | Perf numbers cannot accidentally come from sanitizer builds. |
| P1 | `build/lto-pgo-presets` | Add LTO/PGO presets or docs once benchmarks are stable. | Depends on perf harness. | Presets do not disturb normal dev/debug builds. |
| P2 | `build/package-config` | Improve install/export package shape. | Later; not a runtime blocker. | Namespaced target export works from install tree. |

Recommended next build branch: `build/perf-harness-stabilize`.

### Docs / examples / API ergonomics lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P0 | `docs/concurrency-naming-model` | Canonicalize execution model and `blocking_`/`sync_`/`async_` semantics. | Docs-only; should merge first. | New code review guidance points to one document. |
| P0 | `docs/parallel-priority-plan` | Add this file. | Done by this patch. | Future branches can pick component queues without central replanning. |
| P1 | `docs/examples-compile-ci` | Ensure examples compile in CI. | Can run with build branch. | Every documented example is built/tested. |
| P1 | `docs/json-boundary-guide` | Explain JSON provider trait boundary and why parser work is later. | Depends on JSON boundary branch shape. | Route authors know where JSON dependencies are allowed. |
| P2 | `docs/release-blockers` | Maintain release-blocker checklist. | Later, after P0/P1 branches settle. | Checklist includes security, docs, perf harness, fuzzing, alias removal. |

Recommended next docs branch: `docs/concurrency-naming-model`.

### API naming / alias cleanup lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P1 | `docs/naming-audit` | Document old names that should eventually become `blocking_`, `sync_`, or `async_`. | Docs-only; no renames. | Audit exists without code churn. |
| P2 | Component-local rename branches | When already editing a component, add clearer names and keep aliases. | Only inside the active component branch. | New call sites use new names; aliases remain. |
| R | `release/remove-aliases` | Remove compatibility aliases and stale names. | Must be last pre-release cleanup. | All code/tests/docs use final names; no feature branches depend on aliases. |

Alias elimination is the last release-prep task, not a modernization task.

## Suggested immediate branch fan-out

These branches can start from the same base with low conflict risk:

1. `docs/concurrency-naming-model`
   - Touches docs only.
   - Clarifies ring-thread handler execution and naming model.

2. `worker/no-wait-bridge`
   - Highest implementation priority.
   - Serial gate for remaining worker runtime work.

3. `auth/password-hash-replacement`
   - Security-critical and mostly independent.
   - Coordinate only where login routes or DB migrations are touched.

4. `json/boundary-traits`
   - Enables later JSON cleanup without choosing final parser.
   - Do before parser/DOM/reflection work.

5. `build/perf-harness-stabilize`
   - Enables measured HTTP/uring/worker perf changes.
   - Avoids further unmeasured low-level tweaks.

6. `uring/setup-flag-fallback-log`
   - Small low-conflict correctness/observability improvement.

7. `uring/iopoll-storage-ring`
   - Independent from HTTP send path and worker runtime.

## Deferred work

- `worker/p2300-prototype`: worthwhile but too broad until V2 runtime migration is
  finished and benchmark harness is stable.
- `uring/recv-zc`: defer implementation until target kernels are stable; prepare the
  abstraction earlier.
- Full JSON parser/arena DOM/reflection serde: important, but do not start until
  `json/boundary-traits` makes provider replacement local.
- HTTP/2/HTTP/3: not before handler/runtime semantics stabilize.
- Broad public API rename: not before component internals settle.
- Alias removal: final release cleanup only.

## Release blockers snapshot

- Worker runtime has no hidden compatibility wait path outside an explicitly named
  adapter.
- Password hashing is production-grade and migration-aware.
- JSON provider usage is isolated enough that replacing the backend is not a route
  rewrite.
- Perf harness exists and no perf claim lands without same-machine benchmark notes.
- Public docs state concurrency, handler execution, and naming semantics correctly.
- Examples compile in CI.
- Hardened defaults are documented and tested.
- Aliases are removed only after all above blockers are complete.
