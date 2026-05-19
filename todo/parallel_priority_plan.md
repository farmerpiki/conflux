# Parallel Implementation Priority Plan

Date: 2026-05-15

This document converts the current code/doc state into branchable work lanes. It is
intentionally not a single linear backlog: the project is still pre-release, but
the remaining work now spans mostly independent components. Use this file to pick
parallel branches without repeatedly re-deciding global priority.

## Reading order

1. Use `todo/proposal_state.md` to decide whether a TODO/proposal is still open,
   implemented, deferred, or historical.
2. Keep `proposals/perf_ideas.md` as the current io_uring/perf inventory.
3. Keep `/home/claudiu/conflux_dev/api_traps.md` as the migration-trap
   reference, but apply the project-specific clarifications in this file first
   where they differ.
4. Use this file for branch selection and merge sequencing after checking the
   proposal-state index.
5. Treat TODO/proposal files as potentially stale unless this file,
   `todo/proposal_state.md`, or code state confirms them.
6. Do not start alias elimination until the release-cleanup lane says so.

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

- Correctness: keep recv-bundle/server-lifetime churn on a single owner branch
  until the current E2E lane is verified green. The bundle recycling bug is marked
  fixed in `todo/io_uring_remaining.md`, but future recv arming, provided-buffer
  ring, close/recycle, or connection-lifetime changes still need one owner.
- Worker runtime: no background-ingestion surface exists in this tree, so do not start
  `worker/background-ingestion-runtime` from this snapshot. `WorkPool` now has opt-in
  queue/park/wake counters plus admission/local/steal lock-contention probes for profiling;
  use them before changing admission/local-deque locking. Carrier internals now use
  `blocking_join(...)` instead of the legacy `join(...)` alias for blocking
  conversion/admission/drain paths. Continue moving any remaining compatibility surface
  behind explicitly named `sync_`/`blocking_` APIs.
- Security: password-hash replacement, secret-config cleanup, and session/token audit are landed; any further auth work is now follow-up only.
- JSON boundary: provider-neutral request/response/app route helper seams are now
  in place. Remaining JSON work should add fixtures/design/prototypes without
  reintroducing concrete provider dependencies into HTTP/app framework code.
- HTTP/io_uring: SEND_ZC edge measurement is landed; validate remaining threshold tuning; IOPOLL is landed;
  defer RECV_ZC implementation until kernel support is stable, but prepare the recv
  abstraction so the later branch is narrow.
- Perf/CI: fuzz-smoke and benchmark/profiling harnesses exist; next CI gap is an explicit benchmark regression budget/policy before accepting more low-level perf claims. `todo/proposal_state.md` records which perf proposals are implementation-complete vs measurement-only.
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
      -> worker/taskpromise-frame-pool
      -> worker/background-ingestion-runtime [deferred until app ingestion surface exists]
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
      -> worker/queue-contention-followup
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
| DEFER | `worker/background-ingestion-runtime` | Merge/migrate background ingestion runtime surface onto the worker runtime model. | Do not start from this snapshot; no background-ingestion implementation exists to migrate. | Background ingestion uses the same runtime conventions as other worker tasks once a surface exists. |
| DONE | `worker/taskpromise-frame-pool` | Extended `CONFLUX_WORK_CORO_FRAME_POOL` coverage to `Task<T>` promise frames with a process-lifetime mmap bucket pool; `EagerChain` keeps the thread-local LIFO arena because it never externally suspends. | Completed on top of `worker/no-wait-bridge`. | Small/medium `Task<void>` frames avoid global `::operator new` in steady-state when the pool option is enabled; sanitizer builds keep the safe PMR fallback. |
| DONE | `worker/queue-contention-profile` | Added opt-in `CONFLUX_WORK_QUEUE_STATS` counters for admission, local/inject queues, steal scans, wake/park/futex paths, plus raw NDJSON queue counters in `workpool_enqueue_dequeue`. | Completed on top of worker frame-pool slice; instrumentation is disabled by default. | `benchmarks/notes/worker_queue_contention_profile.md` documents the no-lock-removal decision and profiling command. |
| DONE | `worker/queue-contention-followup` | Added admission/local/steal mutex contention probes and burst/local-fanout benchmark variants so queue activity can be separated from actual lock blocking. | Completed on top of `worker/queue-contention-profile`; no scheduler semantics changed. | `workpool_enqueue_dequeue` emits `external_burst` and `local_fanout` variants plus `*_lock_contentions` counters in the raw queue object. |
| DONE | `worker/carrier-blocking-join-surface` | Carrier blocking conversion/admission/drain paths call `root::blocking_join(...)` directly instead of the legacy `root::join(...)` alias. | Completed on top of `worker/no-wait-bridge`; source/docs only. | `src/work/carrier_*` has no `root::join(...)` call sites; docs identify carrier `from_*`, `Scope::admit`, and `DroppableSlot::wait` as blocking-join surfaces. |
| P3 | `worker/p2300-prototype` | Prototype P2300/io_uring scheduler behind an experimental target. | Do not mix with active V2 runtime migration. | Prototype compiles separately; no public API commitment. |

Recommended next worker branch: none. The 2026-05-18 queue evidence supports `no_stealing` only as an opt-in bounded-offload mode and rejects a default flip because local backlog redistribution needs stealing.

### HTTP server / routing / handler API lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| DONE | `http/handler-execution-docs` | Updated HTTP docs/examples wording to match ring-thread execution and naming policy while landing `docs/concurrency-naming-model`. | Docs/examples only. | No docs imply arbitrary sync handlers are offloaded automatically; async examples use owning request types. |
| DONE | `http/sendzc-mapped-edge` | Mapped-file header+body SEND_ZC edge case is covered; header send is split from large-body SEND_ZC and the bench covers the adaptive threshold across plain/mapped bodies at the boundary sizes. | Landed on the HTTP send path; avoid reopening unless the send-path measurement changes again. | Mapped-file send path has explicit measured behavior and fallback rationale. |
| P1 | `http/send-threshold-bench` | Capable-host summary reviewed; no default change. Plain SEND_ZC rows were all copied, TLS bypassed correctly, and static-file rows labelled `mapped` did not hit mapped attempts. | Depends on perf harness and host kernel/NIC behavior. | Next useful work is benchmark/path coverage cleanup or a true mmap-response SEND_ZC run, not threshold tuning. |
| DONE | `http/limits-defaults` | Hardened HTTP limits/defaults audit landed: INI/default config exposes body, request-line, header-line, header-count, aggregate-header, chunk-count, request-timeout, TLS-sniff-timeout, and HTTP/3 body caps; HTTP/1 parser now enforces incomplete request-line/header-line/count caps before the final header terminator. | Completed; avoid reopening unless defaults policy changes. | Config/parser docs and tests cover the limits. |
| P2 | `http/ring-layout-c2c-verify` | Verify `Ring` hot/cold field grouping with `perf c2c`; pad only if measured. | Depends on perf harness; low conflict. | Either measured padding patch or no-change note. |
| P2 | `http/examples-route-minimal` | Add/keep a minimal route/JSON response example that stays under the target ceremony budget. | Examples/docs; can run parallel after JSON boundary shape is stable. | Example compiles in CI. |
| P3 | `http2/core-prototype` | HTTP/2 core exploration. | Start only after handler/runtime model stops moving. | Separate target or feature flag; no core churn. |

Recommended next HTTP branch: clean up SEND_ZC benchmark/path coverage if continuing send work; otherwise leave thresholds unchanged and move to the next non-SEND_ZC P1/P2 item.

### Low-level io_uring / socket / file I/O lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| Done | `uring/setup-flag-fallback-log` | Log requested-vs-active setup flags after EINVAL stripping. | Landed in the low-level `conflux.uring` setup-flag helpers and the HTTP startup banner delegates to that shared path. | Startup log makes `NO_SQARRAY`, `SUBMIT_ALL`, `CQE_MIXED`, etc. requested/active/stripped status visible. |
| Done | `uring/iopoll-storage-ring` | Added storage-only `IORING_SETUP_IOPOLL` support for O_DIRECT file rings. | Touches file I/O/ring init; independent of HTTP send path. | IOPOLL cannot mix with sockets; config makes storage-only scope explicit; tests cover fallback and dedicated storage ring behavior. |
| DONE | `uring/sendzc-edge-measurement` | Added focused SEND_ZC counter splits and benchmark output around plain, mapped-file, submit-fallback, copied-notification, adaptive-disable, and TLS-bypass paths. | Depends on perf harness; independent of auth/json. | `conflux_send_zc_bench` is recorder-discoverable and emits per-variant SEND_ZC/TLS fallback counters in NDJSON. |
| DONE | `uring/recv-abstraction-for-zc` | Added `RecvPayload` as the CQE recv ownership boundary with explicit storage/pinning descriptors and HTTP/server adoption. | Landed after SEND_ZC measurement; behavior stays on provided buffer rings. | Existing classic, recv-bundle, and incremental behavior remains unchanged; RECV_ZC has a named future backend slot. |
| DONE | `uring/sendzc-cqe-lifecycle-test` | Extracted SEND_ZC data/notification CQE transition accounting into a deterministic helper and added direct tests. | Adjacent to HTTP send path; no kernel behavior change. | Partial, complete, copied-notification, no-notification, ENOMEM, and close-after-notification transitions are covered without a live ring. |
| P3 | `uring/recv-zc` | Implement `IORING_OP_RECV_ZC`. | Wait for stable target kernel support and abstraction branch. | Feature-gated, runtime-probed, clear fallback. |

Recommended next uring branch: defer `uring/recv-zc` until target kernel support is stable; use `http/send-threshold-bench` for adjacent measured send-path work.

### Template lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P2 | `template/compiled-cache-reload` | Add immutable compiled template values, structured diagnostics, eager directory compile/publish, optional render-check preflight, explicit reload/invalidation, parsed-context render overloads, and opt-in watcher adapter semantics. | Independent of DB/SEND_ZC evidence and JSON impl-unit split, but touches `src/template.cxx`, template tests, benchmarks, CMake component mapping, and possibly file I/O async imports. | Warm render does not rescan/split template expression/filter/macro strings; reload-all builds a full temporary cache, reports parse/compile/link diagnostics, can run caller-provided render checks, and atomically swaps only on success; failed compile/link/render-check keeps old output; watcher support is separate/opt-in and defaults to coalesced full reload; `render_string(...)` remains uncached/cold. |
| P3 | `template/watch-split-cleanup` | Split watcher integration into a dedicated adapter module/target after the compiled reload primitive exists. | Can follow the compiled-cache branch; avoid preserving current per-file mutation semantics as the default. | `conflux.templates` has no `conflux.file_watch` import or watcher member; `conflux::template_watch` exports a template-specific adapter instead of only mapping to raw file-watch. |

Recommended next template branch: `template/compiled-cache-reload` after the current P1 DB/SEND_ZC evidence branches, or earlier if web ergonomics becomes the active focus.

### JSON / serde / app boundary lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| DONE | `json/boundary-traits` | Introduced provider-neutral JSON dump/decode/write traits and split HTTP JSON helpers into boundary-first `*_with<Provider>` APIs plus an isolated native convenience module. | Landed as JSON/HTTP boundary slice; parser internals unchanged. | HTTP JSON request/response helpers no longer require the native provider at the framework seam; direct providers can stream response chunks. |
| DONE | `json/route-response-writer` | Added provider-neutral `write_with` plus HTTP route response helpers in `conflux.net.http.response_json`. Native provider currently falls back through `dump_json`; direct providers can stream chunks through `write_json(value, opts, sink)`. | Landed as isolated JSON/HTTP helper slice; parser internals untouched. | Response path serializes through adapter; direct writer providers avoid forced intermediate strings. |
| DONE | `json/app-boundary-cleanup` | Added provider-explicit app/router typed JSON route helpers in `conflux.net.http.app_json`; request decode and response serialization now flow through boundary traits without importing the native provider. | Landed after boundary traits; parser internals untouched. | Reusable app/route helpers do not choose a concrete provider; native convenience remains isolated at `conflux.net.http.native_json`. |
| DONE | `json/bench-fixtures` | Added JSON perf/correctness fixtures for route payloads and malformed inputs, plus generated invalid-UTF-8 coverage. | Landed after app-boundary cleanup; parser internals unchanged. | Benchmarks/tests cover strict UTF-8, large numbers, missing/out-of-order keys, duplicate keys, and deep nesting. |
| DONE | `json/parser-dom-design` | Added a documented `JsonDomPolicy` / `parse_dom(...)` prototype facade for view-first, caller-PMR, and arena-backed DOM paths. | Parser internals unchanged; future parser work must stay behind this surface. | Memory model, error model, UTF policy, number policy, object-index policy, and integration API are named and tested. |
| DONE | `json/reflection-serde` | Added optional reflected native boundary provider, mapped boundary decode options onto native DOM/decode policy, and made reflected codecs honor unknown-member policy. | Reflection remains opt-in under `CONFLUX_JSON_REFLECT`; route/app helpers still bind through provider traits. | No macros; no HTTP/app hard provider dependency; reflected provider has tests under the P2996 lane. |
| P2 | `json/impl-unit-split` | Continue splitting `src/json.cxx` into private implementation units while preserving `conflux::json` and `import conflux.json`. | Parser/arena extraction landed; avoid public API renames and JSON feature work in the same branch. | Parser edits now live in `json_parse.cxx`; continue with builder/dump/stream cold bodies and record build evidence on a capable modules toolchain. |
| P3 | `json/schema-pointer-patch` | JSON Pointer/Patch/schema support. | After core boundary/parser shape. | Feature targets separate from core hot path. |

Recommended next JSON branch: continue `json/impl-unit-split` with builder/dump/stream extraction and compile-time evidence; otherwise defer JSON feature work until P0/P1 gates land.

### Auth / security lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| DONE | `auth/password-hash-replacement` | Dedicated password-hash wrapper hardened: Argon2id modular format, CMake-selected linked/runtime Argon2 provider, PBKDF2-SHA256 compatibility fallback, no-allocation PBKDF2 inner loop, verifier-secret `k=1` metadata, bounded KDF concurrency/queueing. | Independent of worker/JSON unless login routes are touched; coordinate DB migration and secret provisioning. | Hashes include algorithm/version/params/pepper flag; tests cover vector/verify/fail/upgrade/pepper/resource limits and optional Argon2id backend path. |
| DONE | `auth/secret-config-cleanup` | Moved password verifier secret plus JWT/cookie/session active+previous secret sources into typed auth config with explicit missing/short-secret errors. | Completed on auth lane; mostly config/auth. | No silent default production secrets; pepper source is separate from password hash storage. |
| DONE | `auth/session-token-audit` | Added JWT session-token policy knobs for required `exp`/`iat`/`jti`, clock skew, max lifetime, negative timestamp rejection, and `jti` revocation hook; documented cookie/session storage boundaries. | Completed on auth lane; no route ergonomics changes. | Tests cover expiry/skew/lifetime/revocation surfaces and docs capture current non-goals. |
| DONE | `auth/rate-limit-hooks` | Added reusable `AuthFailureLimiter`, per-account/query/remote/bearer-token key helpers, metrics snapshots, and a middleware adapter for downstream auth-failure hooks. | Completed on auth lane; no route rewrite required. | Hook points exist; default remains safe/simple; route/account/API-token throttling is documented. |

Recommended next auth branch: none in the current P0/P1 auth lane. If continuing security work, plan a separate `auth/session-store-revocation` branch for persistent session storage / cluster-wide revocation; otherwise return to the global queue (`http/send-threshold-bench` or host execution of `db/pipeline-live-evidence`).

### Build / tests / perf / CI lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| DONE | `build/perf-harness-stabilize` | Make perf harness reproducible: benchmark presets, symbols, fixed inputs, result artifact path, and docs. | Independent; unlocks several perf branches. | Perf presets are benchmark-only `RelWithDebInfo`/no-sanitizer/no-LTO; perf matrix and recorder enforce preset shape; recorder stores manifest, bench-info, cache, logs, and raw NDJSON artifacts; empty/invalid benchmark output fails instead of producing empty DB runs; fixed iteration reuse also derives warmup. |
| DONE | `build/module-fragility-regression` | Added source-shape regression checks and docs for fragile coroutine-adjacent module interfaces. | Build/docs; low conflict. | `build/module-fragility-regression` CTest guards `conflux.net.cancel` as a thin declarations-only interface, keeps private impl units private, and blocks `import std` from `conflux.socket_io.coro`. |
| DONE | `build/ci-sanitizer-perf-split` | Separate sanitizer correctness lanes from release/perf lanes. | Landed as CMake/script/docs guardrail work. | `run-sanitizer-matrix.sh` asserts tests-on/benchmarks-off/LTO-off and the expected sanitizer mix per correctness preset; `bench_record.sh` rejects sanitizer/debug/non-perf recordings unless explicitly waived. |
| DONE | `build/lto-pgo-presets` | Stabilized optimized release/PGO presets after perf harness work: Clang LTO uses ThinLTO, GCC 16 keeps GCC LTO coverage, GCC 15 remains no-LTO, PGO paths are deterministic, and a static CTest guard enforces optimized-preset shape. | Landed as CMake/preset/script/docs work. | `build/optimized-presets` checks release/PGO presets stay unsanitized, explicit, and separate from `perf-*` recording lanes. |
| DONE | `build/package-config` | Install/export package shape now has explicit version ownership, component metadata, requested-component validation, a canonical `conflux::conflux` umbrella alias when available, and package smoke scripts. | Build/docs only. | `build/package-config` statically guards package CMake shape; package smoke validates installed namespaced component targets from a downstream project. |
| DONE | `build/install-tree-smoke` | Added a real downstream install-tree smoke: configure/build/install a fresh dependency-light tree, consume the installed prefix with `find_package(conflux)`, compile/link a module-importing executable, and run it. | Build/docs only; keep separate from CI/fuzz budget changes. | `run-install-tree-smoke.sh` drives the full build/install/consume flow; `run-package-config-smoke.sh` now builds and runs the downstream consumer; opt-in CTest gates exist for both preinstalled and freshly installed prefixes. |
| DONE | `build/bench-regression-budget` | Added DB-backed per-benchmark budgets, `bench_budget_eval`, and `scripts/bench_check_budget.py` for merge-blocking perf comparisons. | Build/scripts/docs; no runtime overlap. | Same-machine baseline/candidate comparisons classify pass/noisy/regression/unbudgeted rows and point failures at recorded artifacts. |

Recommended next build branch: none in the current P0/P1 build lane. Tune benchmark budgets only when host perf artifacts justify changed thresholds.

### Docs / examples / API ergonomics lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| DONE | `docs/concurrency-naming-model` | Canonicalized execution model and `blocking_`/`sync_`/`async_` semantics in `docs/concurrency-naming-model.md`. | Docs-only. | New code review guidance points to one document. |
| DONE | `docs/parallel-priority-plan` | Add and maintain this file. | Docs-only. | Future branches can pick component queues without central replanning. |
| DONE | `docs/todo-state-prune` | Prune stale TODO/proposal state against code before opening more branches. | Docs-only. | Completed items are marked done; active next branches are current. |
| DONE | `docs/examples-compile-ci` | Added `CONFLUX_BUILD_EXAMPLES`, `conflux_examples`, and `examples/compile` CTest build gate. | Build/docs only. | Server examples compile without being executed. |
| DONE | `docs/json-boundary-guide` | Documented provider-neutral modules, native convenience edge, provider shape, and rules for HTTP/app framework code. | Landed with boundary traits. | Route authors know where JSON dependencies are allowed. |
| P2 | `docs/release-blockers` | Maintain release-blocker checklist. | Later, after P0/P1 branches settle. | Checklist includes security, docs, perf harness, fuzz-smoke/JSONTestSuite status, benchmark budget, alias removal. |

Recommended next docs branch: `docs/release-blockers` after recv-bundle/server-lifetime verification is green and the benchmark-budget branch lands.

### API naming / alias cleanup lane

| Priority | Branch | Scope | Parallel safety | Acceptance |
|---|---|---|---|---|
| P1 | `docs/naming-audit` | [x] Document old names that should eventually become `blocking_`, `sync_`, or `async_`. | Docs-only; no renames. | Audit exists in `docs/naming-audit.md` without code churn. |
| DONE | `file/blocking-name-aliases` | Added `blocking_*` aliases for direct caller-thread file I/O, mmap setup, and file-backed JSON parsing. | File/json-file docs/tests/source only; legacy spellings retained. | New alias tests cover `conflux.file_io_sync`, `conflux.file_map`, and `conflux.json.file`; docs identify old names as compatibility spellings. |
| P2 | Component-local rename branches | When already editing a component, add clearer names and keep aliases. | Only inside the active component branch. | New call sites use new names; aliases remain. |
| R | `release/remove-aliases` | Remove compatibility aliases and stale names. | Must be last pre-release cleanup. | All code/tests/docs use final names; no feature branches depend on aliases. |

Alias elimination is the last release-prep task, not a modernization task. The current inventory lives in `docs/naming-audit.md`.

## Suggested immediate branch fan-out

These branches can start from the same base with low conflict risk. Keep
recv-bundle/server-lifetime verification on a single correctness branch and
avoid recv/server lifetime churn elsewhere until it is green. Check
`todo/proposal_state.md` before treating any older proposal as open work.

1. DONE: `db/pipeline-live-evidence`
   - Host PostgreSQL evidence captured: 18 DB integration tests passed and
     `db_pipeline_bench` showed 2.33x median pipeline speedup over plain.
   - DB-only; no follow-up needed unless new DB runtime behavior lands.

2. `http/send-threshold-bench` follow-up
   - Tooling and capable-host evidence exist, but every threshold rollup still had
     zero `ok` pairs: plain candidates were copied, TLS bypassed correctly, and
     static-file rows labelled `mapped` did not hit mapped SEND_ZC attempts.
   - Keep code defaults unchanged. Next useful work is benchmark/path coverage
     cleanup or a true mmap-response SEND_ZC run, not threshold tuning.

3. DONE: `file/file-io-module-split`
   - Source-shape split inside the existing `conflux_file_io` target is already
     landed; do not reopen it as a broad component split.

4. DONE: `http/server-impl-split`
   - Private HTTP server implementation units are already present; do not reopen
     this as broad split work.

5. `json/impl-unit-split`
   - Parser/arena extraction landed; continue private implementation-unit cleanup
     in `src/json.cxx`; keep one public JSON target/import and record compile-time evidence.
   - Do not combine with Pointer/Patch/schema feature work.

6. `worker/queue-contention-measurement`
   - Produce contention evidence before changing local queues or admission locking.
   - Instrumentation/bench notes only unless counters prove a bottleneck.

## Deferred work

- `worker/p2300-prototype`: worthwhile but too broad until V2 runtime migration is
  finished and benchmark harness is stable.
- `uring/recv-zc`: defer implementation until target kernels are stable; prepare the
  abstraction earlier.
- Full JSON parser/arena DOM rewrite and JSON Pointer/Patch/schema feature work:
  defer until reflection/provider experiments and benchmark gates show the
  current facade is too limiting. A source-only JSON implementation-unit split
  is separate P2 ergonomics work and must preserve the public target/import
  shape.
- HTTP/2/HTTP/3: not before handler/runtime semantics stabilize.
- Broad public API rename: not before component internals settle.
- Original modular-build and stream-removal proposals: historical unless their
  `.updated.md` files or `todo/proposal_state.md` list a remaining branch.
- Alias removal: final release cleanup only.

## Release blockers snapshot

- Worker runtime hidden compatibility wait path is isolated behind explicitly
  named `blocking_join(...)`; final release cleanup can remove the legacy `join(...)` alias.
- Password hashing is production-grade and migration-aware.
- JSON provider usage is isolated enough that replacing the backend is not a route
  rewrite.
- Perf harness exists, records preset/cache/log/raw artifacts, rejects accidental non-perf inputs, benchmark regression budgets gate same-machine perf comparisons, and SEND_ZC threshold evidence tooling exists; the latest capable-host threshold summary still produced zero `ok` pairs, so thresholds stay unchanged and the open SEND_ZC work is path coverage rather than tuning.
- Public docs state concurrency, handler execution, and naming semantics correctly.
- Examples compile in CI.
- Hardened defaults are documented and tested.
- Aliases are removed only after all above blockers are complete.
