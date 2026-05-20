# Next Implementation Priorities

> Status: superseded for branch ordering by `todo/proposal_state.md` plus
> `todo/parallel_priority_plan.md`. Keep this file as older rationale for
> boundary/naming policy, not as the first source for selecting the next branch.

This list is intentionally ordered so future patch work can pick the first open
item instead of re-deciding from scratch. It is based on the current `todo/`,
`proposals/`, and top-level design documents in this source snapshot.

## Selection rules

1. Prefer contract-breaking modularity and API-boundary fixes before deeper perf
   work; this is still pre-v1, so API breakage is acceptable when it improves
   ergonomics or performance.
2. Prefer coherent target-split and dependency-boundary chunks over tiny one-target patches, while still avoiding large rewrites.
3. Only implement benchmark-gated performance changes when the benchmark harness
   or measurement target already exists.
4. Defer speculative architecture rewrites until allocation/benchmark data proves
   the current path is the bottleneck.

## Ordered list

0. **Use the current proposal-state index first.**
   - `todo/proposal_state.md` classifies open, implemented, deferred, and
     historical proposals. Do not start work from an older proposal header alone.
   - `todo/parallel_priority_plan.md` remains the branch fan-out document after
     proposal state is known.
   - Prerelease cleanup branches should start from
     `conflux_prerelease_followup_cleanup_plan.md`, not older module-split or
     performance proposal headers.

1. **Keep file-layer boundaries honest.**
   - `file_io_sync` stays POSIX-only.
   - `file_map` stays read-only mapping only.
   - `file_io` stays the async/runtime-backed layer.
   - Make `conflux.uring.handle` and `conflux.uring.completion` explicit in `src/` consumers instead of relying on `file_io` re-exports.
   - [x] `src/` consumers now import `conflux.uring.handle` / `conflux.uring.completion` directly where needed; remaining `file_io` imports are for `FileReader`, `current_file_reader()`, and file-pool types.
   - Leave the broader `file_io` re-export in place for tests/examples until the library split is farther along.
   - Timeout submission now lives in `conflux.uring.timeout`; keep moving other non-file runtime helpers out of `file_io` in the same style.
   - HTTP server parsing/formatting helpers now live in `conflux.net.http_server_helpers`; keep peeling `http_server_impl` toward smaller event-loop/state-machine units and move any remaining duplicated request parsing there.
   - HTTP server ring/config helpers now live in `conflux.net.http_server_config`; keep peeling `http_server_impl` toward the send/recv and dispatch state machines.
   - Next large-module split candidate outside HTTP/JSON: `src/file_io/file_io.cxx`; see `docs/archive/proposals/2026-05/file_io_module_split_proposal.md`. Keep the first implementation branch source-only inside the existing `conflux_file_io` target.
   - Router route-pattern parsing/matching now lives in `conflux.net.router_match`; dispatch helpers now live in `conflux.net.router_dispatch`; keep peeling `router_impl` toward state ownership, and static-route registration now lives in `conflux.net.router_static` / `router_static_impl`.

2. **Apply the blocking/sync/async naming pass.**
   - Inventory is documented in `docs/naming-audit.md`.
   - `blocking_*` for raw syscall-style helpers.
   - `sync_*` for executor-owned non-coroutine chains.
   - `async_*` for coroutine/task APIs.
   - Add aliases component-by-component; remove compatibility names only in the final release cleanup branch.

3. **Defer larger refactors until a boundary slice proves itself.**
   - No P2300 rewrite.
   - No JSON public-target over-splitting. A private JSON implementation-unit
     split is acceptable only as zero-behavior source-shape work.
   - No hidden HTTP auto-offload.
   - No HTTP/static IOPOLL adoption, worker lock replacement, or SEND_ZC
     threshold changes without benchmark evidence.

4. **Only revisit public API aliases after the module splits settle.**
   - Keep `conflux.types` as the working surface for now.
   - Do not widen exports just to clean up signatures early.
