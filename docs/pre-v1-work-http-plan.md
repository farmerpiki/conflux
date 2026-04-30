# Pre-v1 Work and HTTP API Plan

This document records the agreed direction for `conflux.work` and the HTTP
server API before v1. It is a planning artifact, not the current public
contract.

## Constraints

- `liburing` is mandatory. Conflux is Linux/io_uring-first, and removing or
  abstracting away the dependency internally is out of scope for this phase.
- This is pre-v1. Intentional API breakage is allowed when it simplifies the
  future public surface.
- Existing behavior works well and should not be rewired gratuitously. Prefer
  facade and adapter layers first, then internal migration.
- `conflux.work` is in partial migration. Legacy `Flow<T>` remains available
  for existing internals during the transition, but it is not the future public
  model.
- The future easy layer should hide execution placement, io_uring details,
  `DeferredResponse`, and carrier/root separation.
- Advanced users must retain explicit control over where work runs: ring/owner
  fast paths, coroutine tasks, and long-running worker-pool tasks.

## Target Direction

Use `api_traps.md` as a constraint on first-contact ergonomics, not as a demand
to remove the low-level runtime. The public story should become:

- easy HTTP path: small, no io_uring flags, no explicit work pool, no manual
  deferred response object
- preferred async path: root-backed coroutine/task API
- advanced path: explicit placement and lane/pool/ring control
- transitional internals: `Flow<T>` can remain until each subsystem is migrated

## Priority List

Status snapshot (2026-04-29):

- P0: completed
- P1: completed (legacy `Flow<T>` public surface deprecated; warnings temporarily suppressible in build)
- P2: completed
- P3: completed
- P4: completed (docs-vocabulary alignment)
- P5: completed
- P6: completed
- P7: completed

Next active workstream: Phase 7 gradual subsystem migration (DNS/file I/O/DB internals) while keeping compatibility adapters until each slice is migrated.

### P0 - Add an easy HTTP facade

Add a small facade over the existing `Config`, `Router`, and `HttpServer` so
first-contact examples do not expose rings, taskrun flags, fixed buffers, or
work pools.

Candidate shape:

```cpp
auto app = http::App::default_server();
app.get("/", [](Request const &) {
    return Response::text("ok");
});
app.run({.port = 9090});
```

Initial implementation can reuse `Config::low_latency()` or current defaults
internally. `Config` remains public for advanced use.

Measurable win: `examples/hello.cxx` becomes fewer than 12 framework lines and
contains no io_uring flag names.

### P1 - Deprecate `Flow<T>` publicly

Mark legacy `Flow<T>` and its public combinator surface as deprecated in docs
first, and with `[[deprecated]]` where compiler noise is acceptable:

- `Flow<T>`
- `then`
- `flat_then`
- `on_error`
- `on_cancel`
- `run_on`
- `move_to`
- `start_on`
- old `Flow<T>` coroutine examples

Do not rewrite DNS, file I/O, DB, or other internals immediately. Treat
`Flow<T>` as an internal compatibility layer until each subsystem has a scoped
migration.

Measurable win: new users stop learning an API planned for removal.

### P2 - Add one preferred async HTTP handler contract

Keep synchronous handlers working, but introduce a preferred root-backed async
handler shape. The exact names are open, but the easy-layer goal is:

```cpp
router.get("/items/{id}", [](Request req) -> http::Task<Response> {
    auto item = co_await load_item(req.params["id"]);
    co_return Response::json(item);
});
```

The first implementation may adapt this to existing `DeferredResponse`
internals. The important change is that user code no longer constructs
`DeferredResponse`, eventfds, or manual completion objects for normal async
responses.

Measurable win: long-running work in HTTP handlers has an obvious non-blocking
shape.

### P3 - Hide `DeferredResponse` behind helpers

Keep `DeferredResponse` for internals and expert escape hatches, but remove it
from normal examples. Add helpers around existing behavior:

```cpp
return http::defer(pool, [req = req.to_owned()] {
    return Response::json(compute(req));
});
```

Later, coroutine handlers should share this internal path. Document any current
protocol gaps, especially deferred response behavior over HTTP/2.

Measurable win: application code can express delayed responses without owning
eventfd-backed state.

### P4 - Present `conflux.work.root` as the future vocabulary

Keep the three execution models, but document them under one root-based story:

- fast lane: owner/ring-affine operation for minimal SQE/CQE work
- coroutine task: normal async work that can yield
- pool task: long-running or blocking work

Avoid teaching `Chain<T>` and `EagerChain<T>` in easy-layer docs. Keep carrier
documentation available for advanced/experimental use until it is either
promoted or retired.

Measurable win: public docs teach one preferred async vocabulary.

### P5 - Add blocking-handler guardrails

Synchronous HTTP handlers should remain supported, but they need visible
guardrails. Add debug/runtime diagnostics for slow handlers on ring threads,
with a configurable threshold.

This should not reject valid fast sync handlers. It should make accidental DB,
filesystem, sleep, or CPU-heavy work in the ring path visible.

Measurable win: common event-loop blocking mistakes become observable without
breaking existing applications.

### P6 - Split examples by audience

Rewrite default examples to use the easy layer:

- `hello`
- `middleware`
- `sse`
- `static`

Move explicit ring/file-io/runtime examples under an advanced grouping or make
their advanced nature clear in comments and docs.

Measurable win: users copying examples inherit the intended v1-facing API.

### P7 - Document the migration contract

Add or update docs to say:

- `liburing` is required
- `Flow<T>` is deprecated and scheduled for removal after transitional users are
  migrated
- `conflux.work.root` is the future async base
- the HTTP easy layer hides runtime placement by default
- advanced users can still choose lanes, pools, and ring-affine execution
- breaking API changes are expected before v1

Measurable win: contributors and users have one source of truth for expected
breakage.

## Suggested Implementation Phases

### Phase 1 - Easy HTTP facade

- Add the facade types and `run` entry point.
- Rewrite `hello` to use the facade.
- Keep `Router`, `Config`, and `HttpServer` unchanged.
- Verify with the existing HTTP tests and the hello example.

### Phase 2 - `Flow<T>` deprecation

- Add deprecation notes to docs.
- Add deprecation annotations selectively.
- Move or rewrite examples that teach `Flow<T>` as the default async model.
- Avoid changing subsystem internals in this phase.

### Phase 3 - Deferred helper

- Add `http::defer(...)` over the existing `DeferredResponse` mechanism.
- Add tests for success, timeout, and pool rejection.
- Update docs/examples to use the helper instead of constructing
  `DeferredResponse` directly.

### Phase 4 - Root-backed async HTTP handlers

- Define the preferred async handler return type.
- Adapt it internally to the existing deferred-response machinery.
- Add tests proving sync and async handlers compose with middleware and error
  handling.

### Phase 5 - Work documentation cleanup

- Reframe root as the future public vocabulary.
- Mark carrier APIs as advanced/experimental unless promoted.
- Clarify the three execution placement choices without requiring easy-layer
  users to understand them.

### Phase 6 - Guardrails and diagnostics

- Add slow sync-handler diagnostics.
- Add configuration for threshold and enablement.
- Keep defaults conservative enough not to affect benchmarks unexpectedly.

### Phase 7 - Gradual subsystem migration

- Migrate DNS, file I/O, DB, and other `Flow<T>` users one subsystem at a time.
- Keep compatibility adapters only as long as needed.
- Remove `Flow<T>` soon after no internal or public easy-layer path depends on
  it.

## Non-goals For The First Pass

- Removing `liburing`.
- Replacing the working HTTP server internals.
- Rewriting DNS/file I/O/DB away from `Flow<T>` in one pass.
- Making every advanced work/carrier API v1-stable immediately.
- Solving HTTP/2 deferred/coroutine support before the easy async contract is
  established.
