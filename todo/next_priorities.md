# Next Implementation Priorities

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

1. **Keep file-layer boundaries honest.**
   - `file_io_sync` stays POSIX-only.
   - `file_map` stays read-only mapping only.
   - `file_io` stays the async/runtime-backed layer.
   - Make `conflux.uring.handle` and `conflux.uring.completion` explicit in `src/` consumers instead of relying on `file_io` re-exports.
   - Leave the broader `file_io` re-export in place for tests/examples until the library split is farther along.
   - Timeout submission now lives in `conflux.uring.timeout`; keep moving other non-file runtime helpers out of `file_io` in the same style.
   - HTTP server parsing/formatting helpers now live in `conflux.net.http_server_helpers`; keep peeling `http_server_impl` toward smaller event-loop/state-machine units.
   - HTTP server ring/config helpers now live in `conflux.net.http_server_config`; keep peeling `http_server_impl` toward the send/recv and dispatch state machines.
   - Router route-pattern parsing/matching now lives in `conflux.net.router_match`; keep peeling `router_impl` toward dispatch state, and static-route registration still lives in `conflux.net.router_static`.

2. **Apply the blocking/sync/async naming pass.**
   - `blocking_*` for raw syscall-style helpers.
   - `sync_*` for executor-owned non-coroutine chains.
   - `async_*` for coroutine/task APIs.

3. **Defer larger refactors until a boundary slice proves itself.**
   - No P2300 rewrite.
   - No JSON over-splitting until there is a clear module consumer win.
   - No hidden HTTP auto-offload.

4. **Only revisit public API aliases after the module splits settle.**
   - Keep `conflux.types` as the working surface for now.
   - Do not widen exports just to clean up signatures early.
