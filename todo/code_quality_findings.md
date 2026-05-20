# Code Quality Findings TODO

Status: open inventory
Open: public API alias cleanup, naming pass, verification backlog, and
profiling-gated source splits.
Done reference: completed small focused patches and landed split work remain
below only as historical context.

Scope: follow-up work from the code-quality review. Large source-file splits are
tracked here only as dedicated-branch candidates; do not mix them into cleanup,
correctness, or naming patches.

## Current branch / small focused patches

- [x] Basic auth limiter: clamp `BasicAuthOptions::max_failed_clients = 0` to at
  least one stored client so an empty LRU cannot be evicted or corrupted.
  - Regression: dispatch two bad auth attempts with `max_failed_clients = 0`; the
    first returns `401`, the second returns `429`, and the validator is not
    called again.
- [x] Router route metadata: preserve wildcard-tail star notation in
  `Router::route_infos()` (`/files/{*path}` must not degrade to
  `/files/{path}`).
  - Regression: register a wildcard route and assert `path_pattern` keeps the
    `*` while `path_params` still reports the logical name.
- [x] HEAD route dispatch: use the route index for GET-compatible HEAD dispatch
  instead of scanning every registered sync/context route.
  - Regression target: add or keep coverage proving HEAD still resolves GET
    routes and does not dispatch SSE routes.
  - Measurement target: extend `benchmarks/router_bench.cxx` with HEAD-heavy
    route-table cases before making further router dispatch changes.

## Release-polish / API ergonomics

- [ ] Public API alias cleanup (NEXT serious quality branch): remove exported
  shorthand aliases from public signatures and docs after the rest of the
  public surface settles.
  - Severity: highest remaining v1 ergonomics issue; public namespace leakage
    makes first-contact API discovery inconsistent (`Document`, `JsonError`,
    `Router`, `HttpResponse`, `HttpServer`, `Pool`, `Result`, `Params`, etc.
    appear globally and under `conflux::*`).
  - Branch intent: canonicalize public entry points under `conflux::*` and
    stop exporting duplicate global spellings for user-facing types.
  - Suggested rollout:
    1. JSON surface (`Document`, `JsonError`, friends)
    2. HTTP surface (`Router`, `HttpResponse`, `HttpServer`, helpers)
    3. DB surface (`Pool`, `Result`, `Params`, connection/query types)
    4. Final pass across examples/docs/package snippets
  - Guardrails:
    - no compatibility aliases in public pre-release surface
    - update examples/docs in the same patch as signature cleanup
    - add/adjust tests only for rename coverage unless behavior changes
  - Progress: crypto, utils, process, file sync/map, async file I/O, socket I/O,
    net config, selected HTTP helper surfaces, VHost, OpenAPI,
    cookie/JWT/WebSocket/static helpers now use spelled-out standard vocabulary
    at public boundaries.
  - Existing tracker: `todo/contract_fixes.md`.
  - Keep local implementation aliases where they materially improve readability;
    the release blocker is public-boundary leakage, not private shorthand.
  - Update examples/docs in the same branch as each public signature cleanup so
    users see spelled-out `std::string`, `std::string_view`,
    `std::optional`, `std::shared_ptr`, and `std::vector` vocabulary.
- [ ] API naming pass: finish the `blocking_*` / `sync_*` / `async_*` pass only
  after alias cleanup candidates and component boundaries stop moving.
  - Existing trackers: `docs/naming-audit.md`, `todo/contract_fixes.md`, and
    `todo/next_priorities.md`.
  - Avoid compatibility aliases until final release cleanup unless they reduce
    migration churn inside the tree.

## Verification backlog

- [ ] Run the full CMake configure/build/test matrix after installing missing
  local deps (`Catch2`, `liburing`, TLS/compression deps as enabled by preset).
  - Minimum gates: debug GCC/stdc++, debug Clang/libc++, tests-off configure,
    component package smoke, examples compile gate.
- [ ] Run the lightweight repository guards after each branch:
  - `scripts/check_no_std_streams.py`
  - `scripts/check-cmake-source-files.py`
  - `scripts/check-module-interface-regressions.sh`
  - `scripts/check-optimized-presets.sh`
- [ ] Add a router microbenchmark case for dense route tables with HEAD requests
  so future dispatch changes have a regression budget.
- [ ] Keep fuzz/bench CI work in the existing release-blocker track rather than
  attaching it to local correctness patches.

## Dedicated branch backlog only

Do these one branch at a time. Each branch needs its own build/perf baseline,
source-list patch, module-interface guard run, and focused regression tests.
Use `todo/proposal_state.md` for current priority.

- [x] Split `src/file_io/file_io.cxx` into leaf module units inside the existing
  `conflux_file_io` target.
  - Landed as `conflux.file_io.buffers`, `.pipe_pool`, `.reader`, `.iopoll`,
    and `.driver`, with `conflux.file_io` kept as the compatibility umbrella.
  - No new package components were added in this branch.
- [ ] Split `src/json.cxx` into smaller implementation units only when the split
  produces a clear consumer/build/test boundary.
  - Candidate seams: tokenizer/parser, DOM/node storage, streaming reader,
    reflection helpers, stringify/writer, schema/path helpers.
  - Do not split merely by line count; preserve current fast borrowed/moved parse
    paths and PMR allocation behavior.
- [ ] Split `src/net/http_server_impl.cxx` along event-loop/state-machine seams.
  - Candidate seams: accept/connection state, recv/parser progression,
    request dispatch, send queue/SEND_ZC, shutdown/recovery, metrics.
  - Preserve the documented execution model: handlers run on ring threads unless
    users explicitly offload work.
- [ ] Split `src/work/root.cxx` only after profiling task/control-block hot paths.
  - Candidate seams: task promise/control block, scheduler/root ownership,
    cancellation, allocation diagnostics/pools.
  - Keep allocation-counter semantics and pooled control-block behavior intact.
