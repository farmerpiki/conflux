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
  pipeline path is mostly async, and the remaining root blocking wait path is
  isolated behind the explicitly named `blocking_join(...)` compatibility seam.
  Carrier blocking joins now call that explicit seam directly; legacy `join(...)`
  remains only as a public compatibility alias until release cleanup.
- Module CMI fragility has a proven mitigation: thin exported interfaces plus
  coroutine-heavy implementation units, especially around cancellation/socket/coro
  surfaces.
- API naming policy is now clarified:
  - `blocking_`: raw/blocking syscall-style helpers such as wrappers around
    `::write(fd, ...)`.
  - `sync_`: executor/task APIs that run on the task/executor model but expose a
    non-coroutine success/failure surface.
  - `async_`: coroutine APIs.
- Perf harness initial stabilization is in place: dedicated symbolized `perf-*`
  presets build recordable benchmarks without sanitizers/LTO, the recorder keeps
  manifest/bench-info/raw NDJSON artifacts, and HTTP/file/worker benchmark
  commands are documented.

### Main gaps still worth implementing

- Worker runtime: finish background ingestion runtime convergence when that app surface is present;
  the provided library tree currently has no background-ingestion implementation to migrate.
  `WorkPool` now has opt-in queue/park/wake counters for contention profiling; use them before changing admission/local-deque locking.
  Carrier internals now use `blocking_join(...)` instead of the legacy `join(...)`
  alias for blocking conversion/admission/drain paths. Continue moving any remaining
  compatibility surface behind explicitly named `sync_`/`blocking_` APIs.
- Security: password-hash replacement is landed; finish secret-config cleanup and session/token audit before widening API work.
- JSON boundary: isolate app/framework JSON usage behind traits/adapters before
  designing a new parser/DOM. The parser work is important, but the boundary cleanup is
  the prerequisite that keeps it branchable.
- HTTP/io_uring: finish SEND_ZC edge cases and measurement; IOPOLL is landed;
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
| DONE | `worker/no-wait-bridge` | Root blocking outcome wait isolated behind `blocking_join(...)`; legacy `join(...)` delegates as an alias. | Completed; avoid reopening outside release alias cleanup. | No direct blocking outcome wait calls outside `blocking_join_compatibility_adapter`; async task bodies use ready callbacks / `co_await`. |
| DONE | `docs/concurrency-naming-model` | Added canonical concurrency/naming review guide and linked it from policy/API docs. | Docs-only. | Docs state HTTP handlers run on ring threads, no hidden offload normalization exists, and review guidance points to one document. |
| P1 | `worker/background-ingestion-runtime` | Merge/migrate background ingestion runtime surface onto the worker runtime model. | Depends on `worker/no-wait-bridge`; should not touch auth/json. | Background ingestion uses the same runtime conventions as other worker tasks. |
| DONE | `worker/taskpromise-frame-pool` | Extended `CONFLUX_WORK_CORO_FRAME_POOL` coverage to `Task<T>` promise frames with a process-lifetime mmap bucket pool; `EagerChain` keeps the thread-local LIFO arena because it never externally suspends. | Completed on top of `worker/no-wait-bridge`. | Small/medium `Task<void>` frames avoid global `::operator new` in steady-state when the pool option is enabled; sanitizer builds keep the safe PMR fallback. |
| DONE | `worker/queue-contention-profile` | Added opt-in `CONFLUX_WORK_QUEUE_STATS` counters for admission, local/inject queues, steal scans, wake/park/futex paths, plus raw NDJSON queue counters in `workpool_enqueue_dequeue`. | Completed on top of worker frame-pool slice; instrumentation is disabled by default. | `benchmarks/notes/worker_queue_contention_profile.md` documents the no-lock-removal decision and profiling command. |
| DONE | `worker/carrier-blocking-join-surface` | Carrier blocking conversion/admission/drain paths call `root::blocking_join(...)` directly instead of the legacy `root::join(...)` alias. | Completed on top of `worker/no-wait-bridge`; source/docs only. | `src/work/carrier_*` has no `root::join(...)` call sites; docs identify carrier `from_*`, `Scope::admit`, and `DroppableSlot::wait` as blocking-join surfaces. |
| P3 | `worker/p2300-prototype` | Prototype P2300/io_uring scheduler behind an experimental target. | Do not mix with active V2 runtime migration. | Prototype compiles separately; no public API commitment. |

Recommended next worker branch: `worker/background-ingestion-runtime` if the app ingestion surface is present; otherwise leave the worker lane idle until measured queue contention justifies a follow-up locking/scheduling patch. Any further carrier blocking-name cleanup should be additive alias work only, not release alias removal.

### HTTP server / routing / handler API lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| DONE | `http/handler-execution-docs` | Updated HTTP docs/examples wording to match ring-thread execution and naming policy while landing `docs/concurrency-naming-model`. | Docs/examples only. | No docs imply arbitrary sync handlers are offloaded automatically; async examples use owning request types. |
| P1 | `http/sendzc-mapped-edge` | Finish mapped-file header+body SEND_ZC edge case or document why fallback remains. | Touches HTTP send path; avoid concurrent send-buffer refactors. | Mapped-file send path has explicit measured behavior and fallback rationale. |
| P1 | `http/send-threshold-bench` | Validate/adapt SEND_ZC thresholds under realistic HTTP load. | Depends on perf harness. | Threshold defaults are backed by benchmark notes; counters remain exposed. |
| DONE | `http/limits-defaults` | Hardened HTTP limits/defaults audit landed: INI/default config exposes body, request-line, header-line, header-count, aggregate-header, chunk-count, request-timeout, TLS-sniff-timeout, and HTTP/3 body caps; HTTP/1 parser now enforces incomplete request-line/header-line/count caps before the final header terminator. | Completed; avoid reopening unless defaults policy changes. | Config/parser docs and tests cover the limits. |
| P2 | `http/ring-layout-c2c-verify` | Verify `Ring` hot/cold field grouping with `perf c2c`; pad only if measured. | Depends on perf harness; low conflict. | Either measured padding patch or no-change note. |
| P2 | `http/examples-route-minimal` | Add/keep a minimal route/JSON response example that stays under the target ceremony budget. | Examples/docs; can run parallel after JSON boundary shape is stable. | Example compiles in CI. |
| P3 | `http2/core-prototype` | HTTP/2 core exploration. | Start only after handler/runtime model stops moving. | Separate target or feature flag; no core churn. |

Recommended next HTTP branch: `http/sendzc-mapped-edge`, then `http/send-threshold-bench`.

### Low-level io_uring / socket / file I/O lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| Done | `uring/setup-flag-fallback-log` | Log requested-vs-active setup flags after EINVAL stripping. | Landed in the low-level `conflux.uring` setup-flag helpers and the HTTP startup banner delegates to that shared path. | Startup log makes `NO_SQARRAY`, `SUBMIT_ALL`, `CQE_MIXED`, etc. requested/active/stripped status visible. |
| Done | `uring/iopoll-storage-ring` | Added storage-only `IORING_SETUP_IOPOLL` support for O_DIRECT file rings. | Touches file I/O/ring init; independent of HTTP send path. | IOPOLL cannot mix with sockets; config makes storage-only scope explicit; tests cover fallback and dedicated storage ring behavior. |
| P1 | `uring/sendzc-edge-measurement` | Add focused benchmark/counters around SEND_ZC fallback paths. | Depends on perf harness; independent of auth/json. | Bench output can decide mapped-file/TLS fallback policy. |
| P2 | `uring/recv-abstraction-for-zc` | Refactor recv buffer ownership so RECV_ZC can slot in later. | Can run before kernel support, but avoid changing behavior. | Existing recv behavior unchanged; abstraction names lifetime/pinning requirements. |
| P3 | `uring/recv-zc` | Implement `IORING_OP_RECV_ZC`. | Wait for stable target kernel support and abstraction branch. | Feature-gated, runtime-probed, clear fallback. |

Recommended next uring branch: `uring/sendzc-edge-measurement`, after the current setup-flag fallback/log patch and storage-ring work merge.

### JSON / serde / app boundary lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P0 | `json/boundary-traits` | Introduce a thin JSON provider/serde trait boundary without changing parser implementation. | Touches JSON-facing app/route modules; should merge before route JSON cleanup. | App/framework code stops directly depending on one concrete JSON representation at major boundaries. |
| DONE | `json/route-response-writer` | Added provider-neutral `write_with` plus HTTP route response helpers in `conflux.net.http.response_json`. Native provider currently falls back through `dump_json`; direct providers can stream chunks through `write_json(value, opts, sink)`. | Landed as isolated JSON/HTTP helper slice; parser internals untouched. | Response path serializes through adapter; direct writer providers avoid forced intermediate strings. |
| P1 | `json/app-boundary-cleanup` | Clean remaining app-level JSON usage behind the trait. | Depends on boundary traits; can split by route group. | No new JSON provider lock-in in app code. |
| P2 | `json/parser-dom-design` | Produce design/prototype for view-first parser + arena-backed DOM + reflection serde. | Do not implement broad parser while app boundaries are still concrete. | Prototype/design names memory model, error model, UTF/number policy, and integration API. |
| P2 | `json/bench-fixtures` | Add JSON perf/correctness fixtures for route payloads and malformed inputs. | Can run parallel with design; low conflict. | Benchmarks include strict UTF-8, large numbers, missing/out-of-order keys, duplicate keys, deep nesting. |
| P3 | `json/reflection-serde` | C++26 reflection or PFR bridge under the stable trait shape. | Depends on trait and parser/DOM decision. | No macros, no hard JSON provider dependency, clear compile-time cost measurement. |
| P3 | `json/schema-pointer-patch` | JSON Pointer/Patch/schema support. | After core boundary/parser shape. | Feature targets separate from core hot path. |

Recommended next JSON branch: `json/boundary-traits`.

### Auth / security lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P0 | `auth/password-hash-replacement` | [x] Dedicated password-hash wrapper hardened: Argon2id modular format, CMake-selected linked/runtime Argon2 provider, PBKDF2-SHA256 compatibility fallback, no-allocation PBKDF2 inner loop, verifier-secret `k=1` metadata, bounded KDF concurrency/queueing. | Independent of worker/JSON unless login routes are touched; coordinate DB migration and secret provisioning. | Hashes include algorithm/version/params/pepper flag; tests cover vector/verify/fail/upgrade/pepper/resource limits and optional Argon2id backend path. |
| P1 | `auth/secret-config-cleanup` | Move `PasswordHashOptions::verifier_secret`, session/JWT/cookie secrets, and rotation policy into typed config with explicit missing-config errors. | Depends on password wrapper shape; mostly config/auth. | No silent default production secrets; pepper source is separate from password hash storage. |
| P1 | `auth/session-token-audit` | Audit session/token creation, expiry, storage, revocation, and error surfaces. | Can run after password branch; avoid route ergonomics changes. | Threat model notes and tests for expiry/revocation. |
| P2 | `auth/rate-limit-hooks` | Extend abuse controls beyond the default Basic-auth failed-attempt limiter: login form/account throttles, API-token throttles, metrics, and policy hooks. | Can run parallel with HTTP limits if interfaces are stable. | Hook points exist; default remains safe/simple; route/account-level throttling is documented. |

Recommended next auth branch: `auth/secret-config-cleanup`, then `auth/session-token-audit`; keep broader `auth/rate-limit-hooks` for account/API policies beyond Basic-auth defaults.

### Build / tests / perf / CI lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P0 | `build/perf-harness-stabilize` | Make perf harness reproducible: benchmark presets, symbols, fixed inputs, result artifact path, and docs. | Independent; unlocks several perf branches. | **Initial slice done:** HTTP/file/worker benchmark commands are documented; recorder emits manifest/bench-info/raw artifacts; perf presets provide symbolized non-sanitizer builds. |
| P0 | `build/module-fragility-regression` | Add/keep regression docs/tests for thin-interface module pattern around coroutine-heavy modules. | Build/docs; low conflict. | Agents stop reintroducing heavy coroutine bodies into fragile module interfaces. |
| P1 | `build/ci-sanitizer-perf-split` | Separate sanitizer correctness lanes from release/perf lanes. | CMake/CI only. | Perf numbers cannot accidentally come from sanitizer builds. |
| P1 | `build/lto-pgo-presets` | Add LTO/PGO presets or docs once benchmarks are stable. | Depends on perf harness. | Presets do not disturb normal dev/debug builds. |
| P2 | `build/package-config` | Improve install/export package shape. | Later; not a runtime blocker. | Namespaced target export works from install tree. |

Recommended next build branch: `build/perf-harness-stabilize`.

### Docs / examples / API ergonomics lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| DONE | `docs/concurrency-naming-model` | Canonicalized execution model and `blocking_`/`sync_`/`async_` semantics in `docs/concurrency-naming-model.md`. | Docs-only. | New code review guidance points to one document. |
| P0 | `docs/parallel-priority-plan` | Add this file. | Done by this patch. | Future branches can pick component queues without central replanning. |
| Done | `docs/examples-compile-ci` | Added `CONFLUX_BUILD_EXAMPLES`, `conflux_examples`, and `examples/compile` CTest build gate. | Build/docs only. | Server examples compile without being executed. |
| P1 | `docs/json-boundary-guide` | Explain JSON provider trait boundary and why parser work is later. | Depends on JSON boundary branch shape. | Route authors know where JSON dependencies are allowed. |
| P2 | `docs/release-blockers` | Maintain release-blocker checklist. | Later, after P0/P1 branches settle. | Checklist includes security, docs, perf harness, fuzzing, alias removal. |

Recommended next docs branch: `docs/json-boundary-guide`, after `json/boundary-traits` lands.

### API naming / alias cleanup lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P1 | `docs/naming-audit` | [x] Document old names that should eventually become `blocking_`, `sync_`, or `async_`. | Docs-only; no renames. | Audit exists in `docs/naming-audit.md` without code churn. |
| P2 | Component-local rename branches | When already editing a component, add clearer names and keep aliases. | Only inside the active component branch. | New call sites use new names; aliases remain. |
| R | `release/remove-aliases` | Remove compatibility aliases and stale names. | Must be last pre-release cleanup. | All code/tests/docs use final names; no feature branches depend on aliases. |

Alias elimination is the last release-prep task, not a modernization task. The current inventory lives in `docs/naming-audit.md`.

## Suggested immediate branch fan-out

These branches can start from the same base with low conflict risk:

1. `worker/background-ingestion-runtime`
   - Next worker implementation priority after the no-wait bridge isolation.
   - Converges background ingestion onto the same runtime conventions.

2. `auth/secret-config-cleanup`
   - Builds on the password-hash wrapper shape.
   - Move secrets/pepper/session config behind typed missing-config errors.

3. `uring/sendzc-edge-measurement`
   - Independent from HTTP send path and worker runtime.

4. `json/boundary-traits`
   - Enables later JSON cleanup without choosing final parser.
   - Do before parser/DOM/reflection work.

5. `build/perf-harness-stabilize`
   - Enables measured HTTP/uring/worker perf changes.
   - Avoids further unmeasured low-level tweaks.

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

- Worker runtime hidden compatibility wait path is isolated behind explicitly
  named `blocking_join(...)`; final release cleanup can remove the legacy `join(...)` alias.
- Password hashing is production-grade and migration-aware.
- JSON provider usage is isolated enough that replacing the backend is not a route
  rewrite.
- Perf harness exists and no perf claim lands without same-machine benchmark notes.
- Public docs state concurrency, handler execution, and naming semantics correctly.
- Examples compile in CI.
- Hardened defaults are documented and tested.
- Aliases are removed only after all above blockers are complete.
