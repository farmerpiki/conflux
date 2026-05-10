# conflux::db — Remaining Work

_As of 2026-04-28. Completed items stripped; this tracks only open work._

---

## Done (for reference)

P1, P2, P3, P5, P6(spec), P8, P9, P10, P11, P11b, P12, P13, P15, P16 — all
landed on the `db` branch.

---

## Open

### P7 — Pipeline mode

**Prerequisite:** P2 + P3 stable (both landed). Ready to implement.

**Status update (2026-04-29):** baseline implementation landed in
`src/db/connection.cxx`:
- `Connection::pipeline()`
- `Pipeline::query(...)`
- `Pipeline::exec_cached(...)` (currently delegates to SQL text path)
- `Pipeline::sync()`
- integration coverage added in `tests/db_integration_test.cxx`:
  - `db: pipeline query ordering`
  - `db: pipeline isolates per-query failures`

**Still open for P7 close-out:**
- live Postgres integration run in CI/host with `PG_TEST_CONNINFO` set
- stronger teardown semantics (current destructor is non-blocking and best-effort;
  unresolved queued flows are rejected as `pipeline closed`)
- finalize pipeline implementation details from 2026-04-29 discoveries
  (see contracts below)

**P7 discoveries (2026-04-29):**
- `PQsendQuery` is rejected in pipeline mode by libpq; pipeline send path must
  use pipeline-compatible calls (`PQsendQueryParams`, `PQsendPrepare`,
  `PQsendQueryPrepared`, etc.).
- For pipeline sync loops, `PQgetResult()==nullptr` is not a completion signal;
  completion must key off `PGRES_PIPELINE_SYNC`.
- Result demux must account for each wire message in order. If a query send path
  emits both prepare-ack and execution result, sync demux must consume and map
  both correctly.
- Given sanitizer-stable correctness requirements, the current shipped `Pipeline`
  implementation is a **logical batching barrier** that executes queued items
  in-order through existing `Connection::query` machinery during `sync()`.
  This preserves API/ordering/error contracts and integration-test coverage,
  while true libpq wire-level pipeline mode remains open follow-up work.

**Pipeline contracts (implementation-level):**
- `Connection::pipeline()`:
  - owner-thread only (same lane/thread as `Connection`)
  - only one active pipeline per `Connection` (`pipeline_mode_` guard)
- `Pipeline::query(...)`:
  - owner-thread only
  - rejected while `sync()` is in progress (`query while sync in progress`)
  - enqueues one result promise that is fulfilled/rejected during `sync()`
- `Pipeline::sync()`:
  - owner-thread only
  - non-reentrant (`syncing_` guard)
  - drains queued work in-order and resolves/rejects each queued flow exactly once
  - on sync failure, unresolved queued results are rejected with
    `pipeline sync failed`
- `Pipeline` teardown:
  - destructor is best-effort, non-blocking (`PQexitPipelineMode`)
  - rejects unresolved queued result promises as `pipeline closed`

**Concepts / `requires` constraints status:**
- Pipeline APIs are concrete (non-template), so constraints are primarily
  runtime contracts (thread/phase/state guards).
- Existing generic transaction helpers already enforce compile-time constraints
  via `requires` in `src/db/pool.cxx` (`with_transaction` overloads).

**What:** `PQenterPipelineMode` / `PQpipelineSync` / `PQexitPipelineMode`.
Exposes a `Pipeline` object from `Connection::pipeline()` that batches sends
and demultiplexes results in order.

**Implemented API (as of 2026-04-30):**

```cpp
class Pipeline {
public:
    conflux::work::root::Task<Result> query(string_view sql, Params params = {});
    conflux::work::root::Task<Result> exec_cached(shared_ptr<StatementCache::Entry const>, Params);
    conflux::work::root::Task<void>   sync();
};

// Connection::pipeline() still returns Flow<Pipeline> — migration pending
Flow<Pipeline> Connection::pipeline();
```

- Destructor rejects all pending queries as `pipeline closed`.
- Logical batching barrier: queries enqueued via `query()`, executed in-order during `sync()`.
- True libpq wire pipeline mode (`PQpipelineSync`) is a follow-up.
- Results demultiplexed by insertion order.
- Errors in one sub-query reject that query; remaining queries continue (unless sync fails).
- `Pool::acquire` returns a `Lease`; caller constructs `Pipeline` from `*lease`.

**Performance target:** 5–20× ops/sec for N=100 small INSERTs vs non-pipeline
(benchmark: `db_pipeline_bench.cxx`).

---

### P6 — COPY

**Prerequisite:** none. No in-tree consumer yet — defer until one appears.

**What:** `Connection::copy_in` / `copy_out`.

**API sketch:**

```cpp
namespace root = conflux::work::root;

class CopyIn {
public:
    root::Task<void>    write(span<byte const> chunk);
    root::Task<void>    write_text(string_view line);
    root::Task<int64_t> finish();
    root::Task<void>    abort(string_view reason);
};

class CopyOut {
public:
    root::Task<optional<vector<byte>>> next();
    root::Task<void>                   cancel();
};

Flow<CopyIn>  Connection::copy_in (string_view sql);
Flow<CopyOut> Connection::copy_out(string_view sql);
```

- `CopyOut::next()` uses `PQgetCopyData(async=1)`; buffer freed via
  `PQfreemem` through a custom deleter on the returned vector.
- When P4's multi-shot primitive lands, `CopyOut` may grow a stream-shaped
  overload; `next()` stays as the per-chunk primitive.
- Build benchmark `db_copy_bench.cxx`: `COPY FROM STDIN BINARY` vs
  `exec_prepared` loop. Hypothesis: ≥10× rows/sec.

---

### P4 — Single-row streaming

**Status: blocked on `conflux.work` framework primitive.**

`conflux.work` has no multi-shot stream / channel / queue today (audited
`carrier_streams.cxx`, `carrier_coro.cxx`, `carrier_model_a/b.cxx`,
`carrier_scope.cxx`, `carrier_deadline.cxx`, `carrier_timer.cxx`,
`carrier_flags.cxx`, `root.cxx`). db will not invent one locally.

When the framework ships a multi-shot producer/consumer (working name
`WorkStream<T>`), the implementation becomes:

```cpp
// Future API — not available until WorkStream<T> lands.
WorkStream<Row> Connection::query_stream(string_view sql,
                                         Params params = {},
                                         QueryOptions opts = {});
```

`PQsetSingleRowMode` is called after `PQsendQuery*`; `Result::ok()` re-accepts
`PGRES_SINGLE_TUPLE` (P15 reverted). Workaround until then: `LIMIT`/`OFFSET`
pagination or `DECLARE CURSOR` at the SQL level.

---

### P14 — Namespace rename `conflux::db` → `conflux::pg`

**Status: decision pending.**

If the Postgres-only commitment holds until a SQLite backend is concretely
planned, rename now frees `conflux::db` as the future abstract surface. Cost
is purely textual but touches every call site.

**Options:**
1. Rename to `conflux::pg` now. Clean future abstraction boundary.
2. Stay `conflux::db`, plan backend abstraction when SQLite is scoped.

No code change until a decision is made.

---

## Benchmark gaps (from §5 of the proposal)

| Bench file | Hypothesis | Status |
|---|---|---|
| `db_params_bench.cxx` | ≥3× at param=4 | **Done** (4.1× at param=1, 1.8× at param=4) |
| `db_coro_bench.cxx --binary` | 10–25% reduction (decode-bound) | Not yet |
| `db_pipeline_bench.cxx` | 5–20× at N=100 INSERTs | **Added** (`benchmarks/db_pipeline_bench.cxx`) — currently measures stabilized logical-batching `Pipeline::sync()` path, not libpq wire-level pipeline mode |
| `db_copy_bench.cxx` | ≥10× vs prepared loop | Needs P6 |
| `db_stream_bench.cxx` | TTFB constant in N | Needs P4 (blocked) |
