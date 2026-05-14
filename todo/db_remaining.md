# conflux::db — Remaining Work

_As of 2026-04-28. Completed items stripped; this tracks only open work._

---

## Done (for reference)

P1, P2, P3, P5, P6(spec), P8, P9, P10, P11, P11b, P12, P13, P15, P16 — all
landed on the `db` branch.

---

## Open

### P7 — Pipeline mode

**Status:** implemented on `db/pipeline-wire-mode`.

The shipped pipeline now uses libpq wire-level pipeline mode:

- `Pipeline::sync()` owns one connection job, enters `PQenterPipelineMode`, sends all queued work, issues `PQpipelineSync`, flushes once, then drains until `PGRES_PIPELINE_SYNC`.
- `Pipeline::query(...)` sends `PQsendQueryParams`, so parameterized queries are pipeline-compatible.
- `Pipeline::exec_cached(...)` sends `PQsendPrepare` once per not-yet-prepared statement name in a batch, followed by `PQsendQueryPrepared`; result demux accounts for both prepare ACKs and execution results.
- Result demux is insertion/wire-order based and treats `PGRES_PIPELINE_SYNC` as the completion sentinel; `PQgetResult()==nullptr` is not treated as sync completion.
- Per-query failures reject that query; later commands rejected by PostgreSQL as `PGRES_PIPELINE_ABORTED` reject their own tasks; `sync()` itself succeeds once the sync marker is reached and libpq exits pipeline mode.
- One active pipeline per connection is still enforced. While a `Pipeline` object is alive, ordinary `Connection::query` / `prepare` / `exec_prepared` calls reject instead of interleaving with the pipeline.

**Validation added:**

- `db: pipeline query ordering` continues to cover ordered demux.
- `db: pipeline isolates per-query failures` now exercises libpq abort demux.
- `db: pipeline exec_cached prepares and executes on the wire` covers prepare ACK + execution result demux and same-batch prepared reuse.

**Still recommended before release:**

- Run live PostgreSQL integration in CI/host with `PG_TEST_CONNINFO` set.
- Re-run `db_pipeline_bench.cxx` with `PG_CONNINFO` to replace the old logical-batching baseline numbers.

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
| `db_coro_bench.cxx --binary` | 10–25% reduction (decode-bound) | Added binary-parameter variant (`--binary`) to compare against the text bind path |
| `db_pipeline_bench.cxx` | 5–20× at N=100 INSERTs | **Needs rerun** after `db/pipeline-wire-mode`; benchmark now exercises libpq wire-level pipeline mode |
| `db_copy_bench.cxx` | ≥10× vs prepared loop | Needs P6 |
| `db_stream_bench.cxx` | TTFB constant in N | Needs P4 (blocked) |
