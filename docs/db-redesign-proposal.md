# `conflux::db` — Ergonomics & Performance Redesign Proposal

Audit of `src/db/db.cxx` (1188 LOC, single C++26 module unit). Pairs with the
existing tests (`tests/db_test.cxx`, `tests/db_integration_test.cxx`), the
coroutine micro-bench (`benchmarks/db_coro_bench.cxx`), and the two examples
(`examples/db_basic.cxx`, `examples/db_pool.cxx`). The two callers we have
today are the bench + examples — there is no in-tree user yet, which means
breaking changes are cheap. We should take them.

---

## 1. Summary

- Async path is correct: `PQsendQuery*` + `PQflush` + `poll_add_oneshot` on
  `PQsocket`, never a blocking libpq call on the ring thread. Architecturally
  the hardest part is already right.
- Hot path leaks allocations: `Params` heap-allocates one `std::string` per
  bound value, `query()` takes `string` by value, every fetched column is
  re-parsed from text in `Row::as<T>`. Binary format, prepared-statement
  caching, and small-buffer text bind are all missing.
- `Result` only realises the *last* server-produced result — `PQsetSingleRowMode`
  is unused and there is no streaming/per-row backpressure. Big SELECTs buffer
  the entire result in libpq and then in user code.
- API surface gaps: no transaction helper, no `COPY`, no per-statement timeout,
  no statement-level cancellation handle, no batched/pipeline mode (libpq 14+
  `PQenterPipelineMode`). `Pool` has dead config (`acquire_timeout`,
  `on_acquire`) and no waiter timeout. Type safety stops at `Row::as<T>` —
  there is no row-as-tuple, no struct mapping, no compile-time-checked column
  binding.
- Project-fit drift: `db.cxx` is monolithic where `net/` is split (one
  submodule per concept); names like `Params`, `Row`, `Result`, `Connection`
  collide with the existing `conflux::http` vocabulary. Nothing under
  `conflux::db` is reachable through a shared umbrella `conflux.db.*` graph.

---

## 1a. External dependencies [revised 2026-04-28]

The db component depends on the following framework-level pieces that live
*outside* `conflux::db`. Where the dep does not yet exist, we name the gap
and the workaround.

- **Async DNS resolution** — *future framework component, not in db scope.*
  libpq's `PQconnectStart` resolves `host=` synchronously inside the call
  before the first poll (see Verification Report DNS gap). The fix is a
  framework-level non-blocking resolver (will also serve the HTTP client
  and SMTP). Until that lands, db documents the limitation and recommends
  `hostaddr=` or pre-resolved IPs in conninfo. db will *not* ship its own
  resolver, getaddrinfo-on-WorkPool shim, or DNS classifier.

- **`conflux::work` async carriers.** The module-graph the redesign builds
  on. Specifically: legacy `conflux.work` (`Flow<T>`, `FlowSource<T>`,
  `WorkPool`) for current-style Flow returns, and `conflux.work.root` /
  `conflux.work.carrier.*` for the modern carrier types (`Task<T>`,
  `Posted<T>`, `Operation<T>`, `Chain<T>`, `Scope`, `DeadlineScope`,
  `DroppableSlot<T>`, `CoalescingSlot<T>`, `TimerService`,
  `LaneTimerScope`). **There is no multi-shot stream/channel/queue
  primitive in `conflux.work.*`** — see P4 for the consequence.

- **`conflux::file_io`** — `poll_add_oneshot`, `open_async`, `read_into`,
  `timeout_async`. Already present and used by db today.

- **libpq ≥ 18** (system libpq is `dev-db/postgresql-18.3`, `pkg-config
  --modversion libpq` → `18.3`). Pin to system version. P7 pipeline mode
  (added libpq 14) is a non-issue at this floor; same for any 14/15/16/17
  feature. Build-system constraint, not a framework component.

- **Charset.** db enforces UTF-8 only (see §4 / P-encoding). No pluggable
  encoding layer is required from the framework.

---

## 2. Current state

### 2.1 Modules & namespace

- One module `conflux.db`, namespace `conflux::db`.
- Public types: `PgError`, `Row`, `Result`, `Params`, `ConnectParams`,
  `Connection`, `QueryCache`, `PoolConfig`, `Pool`, `Pool::Lease`, libpq
  RAII deleters/aliases.

### 2.2 Threading model

- Each `Connection` is pinned to the thread that received the
  `Connection::connect` future (`owner_ = this_thread::get_id()`). All
  subsequent `query/prepare/exec_prepared/cancel_inflight` calls on that
  connection must run on the same thread; `query()` rejects with PgError
  otherwise (`db.cxx:816-819`). `Pool` is similarly single-thread (`db.cxx:1106`).
- The connection requires a `current_file_reader()` (TLS) at *connect* time
  (`db.cxx:751-754`); subsequent ops use the cached `reader_` pointer captured
  at construction (`db.cxx:498-502`). The pointer is not re-validated.
- A per-connection FIFO queue (`queue_`) serialises ops over the same socket
  (`db.cxx:530-531, 783-805`). At most one op is in flight on the wire.

### 2.3 Async libpq integration

- `PQconnectStart` + `PQconnectPoll`, driven through `FileReader::poll_add_oneshot`
  with `POLLIN`/`POLLOUT` per `PostgresPollingStatusType` (`db.cxx:679-742`).
- After successful connect, `PQsetnonblocking(conn, 1)` is set, and the
  Connection captures the FileReader pointer.
- Send path: `PQsendQueryParams` / `PQsendPrepare` / `PQsendQueryPrepared`
  followed by `PQflush` loop driven by `POLLOUT` poll completions
  (`db.cxx:928-962`).
- Receive path: `POLLIN` → `PQconsumeInput` → `while (!PQisBusy)` →
  `PQgetResult` until null. Only the last non-null result is kept; intermediate
  results from a multi-statement query are silently dropped
  (`db.cxx:964-1028`). On error the loop drains remaining results via
  `PQgetResult` (`db.cxx:1000-1002`).

### 2.4 Param/result encoding

- `Params` stores values as `vector<optional<string>>` plus parallel
  `lengths_/formats_/types_` vectors. All formats are 0 (text). Numbers go
  through `to_chars` into a small stack buffer then are copied into a
  heap-allocated `string` (`db.cxx:415-431`). `add(string_view)` always
  heap-allocates a copy.
- `result_format()` is hard-coded to 0 (text). All scalar conversions go
  through `from_chars` on the text representation, in `Row::as<T>`
  (`db.cxx:201-265`).

### 2.5 Pool

- LIFO idle stack + FIFO `waiters_` deque of `FlowSource<shared_ptr<Lease>>`.
- Lazy growth on `acquire()` until `cfg_.max_connections`; eager fill to
  `cfg_.min_connections` at `Pool::create` (`db.cxx:1079-1084, 1170-1186`).
- `cfg_.acquire_timeout` and `cfg_.on_acquire` are declared but never read.
- Lease destructor returns the connection to the pool unless it failed
  health-check (`Connection::ok()`); otherwise it decrements `total_`.

### 2.6 QueryCache

- Loads `<root>/<name>.psql` lazily, caches the contents as `shared_ptr<string const>`
  behind a `shared_mutex`, with transparent lookup on `string_view`. File I/O
  is synchronous `std::ifstream` (`db.cxx:559-584`) — runs blocking on whatever
  thread invokes `load()`, including the ring lane.

### 2.7 Cancellation

- `Connection::cancel_inflight(WorkPool&)` enqueues a `PQcancel` call onto a
  caller-provided `WorkPool` (because `PQcancel` is blocking)
  (`db.cxx:1044-1073`). The caller must keep the `WorkPool` alive until the
  Flow resolves.

### 2.8 Performance shape (from the bench)

`benchmarks/db_coro_bench.cxx` runs the same 3-row `SELECT generate_series` the
same number of iterations through (a) `block_on(reader, conn->query(...))` and
(b) the same wrapped in a `Task<void>` coroutine that `co_await`s. It reports
ns-per-iter and `coro vs callback` delta. The bench measures end-to-end (DB
round-trip dominates; callback vs coroutine difference is the only thing it
isolates). It does *not* measure: bind-side allocation, fetch-side parse
overhead, prepared-statement reuse, COPY throughput, or pipeline batching.

---

## 3. Findings

### F1. `query(string sql, Params)` forces a heap allocation per call
**Both, blocker.** `Flow<Result> Connection::query(string sql, Params params)`
takes the SQL by value — every call site that has a `string_view` (or a
literal) costs a `std::string` materialisation (`db.cxx:807, 472`). The whole
point of `QueryCache::load()` returning `shared_ptr<string const>` is to share
SQL buffers; the API throws that away by demanding ownership. Same for
`prepare(string name, string sql, ...)` and `exec_prepared(string name, ...)`.
**Evidence:** `db.cxx:472-476`, `examples/db_pool.cxx:55, 61`.

### F2. `Params` always heap-allocates the bound text
**Performance, high.** Every `add(int64_t)`, `add(double)`, `add(bool)`,
`add(string_view)` goes through `vector<optional<string>>` — one `string`
construction per param, plus a `vector` re-grow path. The `cache_dirty_` rebuild
of `values_cache_` is another `vector<char const*>` allocation. For a
4-parameter query, that's ~5 small heap allocs on the bind side alone before
any I/O happens. Bench at row=3 won't see this; bench at high QPS / shallow
rows will. **Evidence:** `db.cxx:361-452`.

### F3. No binary format support
**Performance, high.** `formats_` is wired through but always pushed `0`
(text); `result_format()` returns `0` hard-coded. Numbers are serialised via
`to_chars` and then re-parsed in `Row::as<int64_t>` via `from_chars`. Doubles
lose precision via text round-trip. Binary format would skip both encodes,
skip server-side text formatting, and be marginally smaller on the wire.
**Evidence:** `db.cxx:402-430, 451`.

### F4. No prepared-statement caching layer
**Both, high.** Callers must (a) call `prepare("name", sql)` once, (b) track
that they've prepared it, (c) call `exec_prepared("name", params)`. The
component does not remember which names are prepared on which connection. A
Pool that recycles connections does not auto-prepare a hot statement on a new
connection; callers either re-prepare every time (free latency loss) or risk
"prepared statement does not exist" after a connection failover. There is also
no SQL-text → name interning — every caller invents names by hand.
**Evidence:** `db.cxx:474, 887-905`.

### F5. Result is "last non-null only" — no streaming
**Both, medium.** `drive_consume_loop_` discards earlier results from
multi-statement strings (`db.cxx:964-1008`) and forces the full row set into
memory inside libpq before user code can see anything. Long SELECTs (export
jobs, report dumps) are bounded by libpq's buffer growth. `PQsetSingleRowMode`
is the standard libpq escape hatch and is unused. There is no `Stream<Row>`,
no `Flow<Row>`, no per-row callback.
**Evidence:** `db.cxx:267-359, 964-1028`.

### F6. `Row::as<T>` does runtime text parsing every call
**Both, medium.** Each `as<i64>` / `as<double>` does a `from_chars` on the
text rep. Iterating a 100k-row Result through `as<i64>(0)` does 100k parses.
With binary format these would be a `bswap` + `memcpy`. Type registry stops
at `i32`, `i64`, `double`, `bool`, `string`, `string_view`. No `optional<T>`
overload — callers have to spell `if (row.is_null(c)) ... else row.as<T>(c)`
every site.
**Evidence:** `db.cxx:201-265`. **Tests confirm:** `tests/db_test.cxx:220-271`.

### F7. No transaction abstraction
**Ergonomics, high.** Callers spell `BEGIN` / `COMMIT` / `ROLLBACK` as
literal queries. No RAII guard, no SAVEPOINT helper, no automatic retry on
`is_serialization()` / `is_deadlock()`, despite those classifiers being right
there on `PgError`. Every consumer will reinvent the same retry loop.
**Evidence:** No occurrences of `BEGIN`/`COMMIT` in `db.cxx`. The classifiers
exist (`db.cxx:62-67`) — a strong hint that retry was meant but not built.

### F8. No `COPY` / bulk path
**Performance, blocker for any data-loading consumer.** `COPY ... FROM STDIN
WITH BINARY` is an order of magnitude faster than parameterised INSERTs for
bulk loads. libpq's `PQputCopyData` / `PQputCopyEnd` / `PQgetCopyData` are not
wrapped. Any caller that wants to ingest data is forced into per-row
`exec_prepared`, which the current FIFO single-flight design then serialises
end-to-end. **Evidence:** No `PQputCopy*` references anywhere in the module.

### F9. No pipeline / batched mode
**Performance, medium.** libpq 14+ supports `PQenterPipelineMode` /
`PQpipelineSync`, which lets a client send N queries and read N replies
without a per-query round-trip. The current per-Connection FIFO is exactly the
shape that benefits — many small writes against the same connection currently
pay one full `flush → poll_in → consume → result` cycle each. Pipelining would
amortise that to one cycle per batch.
**Evidence:** No `PQenterPipelineMode` references.

### F10. `Pool` has dead config
**Bug, low — but a smell.** `PoolConfig::acquire_timeout` and
`PoolConfig::on_acquire` are declared (`db.cxx:596-597`) and never read.
`acquire()` enqueues a waiter forever; if `max_connections` is reached and no
lease is returned, the caller's Flow never resolves. There is no cancellation
path on the waiter side.
**Evidence:** `db.cxx:592-598, 1099-1138`.

### F11. `QueryCache` reads files synchronously on the ring thread
**Performance, low.** `std::ifstream` blocks the thread that called `load()`.
The expected pattern is to call `qc.load(name)` from request-handler code,
which is on the ring lane. `file_io` already has `read_async`/`open_async`;
the cache should optionally use them on cache miss. Cache-hit path is fine
(shared_lock + map lookup). **Evidence:** `db.cxx:559-584`.

### F12. `cancel_inflight` requires a caller-supplied `WorkPool`
**Ergonomics, medium.** Asking the caller to materialise a `WorkPool` just to
cancel one query is friction. The cancel pool is also kept alive by the caller
between the request and the eventual `PQcancel` syscall — easy to mismanage.
At minimum, `Pool` (or `Connection`) should own a tiny shared cancel pool
internally.
**Evidence:** `db.cxx:1044-1073`, `tests/db_integration_test.cxx:264-265`
(test has to construct one).

### F13. `Row::get(string_view col)` does a stack copy + libpq lookup per call
**Performance, low.** `fnumber_sv_` does a 128-byte stack-copy + NUL-terminate
+ `PQfnumber` linear search through column names every call site
(`db.cxx:128-139, 192-199`). For an iteration that touches columns by name in
a hot loop, that's `cols × rows` linear searches. There is no caller-side
cached column index.

### F14. No per-statement timeout / deadline propagation
**Both, medium.** `Connection::query` has no deadline parameter. The only
timeout is `ConnectParams::connect_deadline` (used during the connect dance
only). Callers either accept that a query can hang until the libpq socket
breaks, or they have to wrap the Flow in their own race-with-timer pattern,
or they use `cancel_inflight` from outside. None of those are first-class.

### F15. `Connection::query` returns `Flow<Result>` even on success-with-error-status
**Ergonomics, low.** `drive_consume_loop_` rejects the Flow on `PGRES_FATAL_ERROR`,
which is right. But `Result::ok()` accepts `PGRES_TUPLES_OK || PGRES_COMMAND_OK ||
PGRES_SINGLE_TUPLE`. Today, with single-row mode disabled, `PGRES_SINGLE_TUPLE`
cannot occur — that's dead code that prefigures a feature that isn't there.
**Evidence:** `db.cxx:289-292`.

### F16. Module organisation
**Ergonomics, low.** `db.cxx` is a single 1188-line monolith covering errors,
params, rows, results, connection, pool, query cache, libpq plumbing. The
sibling `net` component is split into ~30 submodules under `conflux.net.*`
with an umbrella `conflux.net.http`. `conflux.db` should have at minimum
`conflux.db.types`, `conflux.db.connection`, `conflux.db.pool`,
`conflux.db.query_cache`, plus an umbrella `conflux.db`. This also helps
build-graph parallelism. **Evidence:** `src/net/http.cxx` (umbrella pattern)
vs `src/db/db.cxx` (single unit).

### F17. Naming collision risk with `conflux::http`
**Ergonomics, low.** `conflux::db::Connection` and `conflux::db::Pool` are
fine when fully qualified, but a future async HTTP client will want
`conflux::http::Connection` / `Pool`. Today the http client lives as
`HttpClient` deliberately to avoid that. The db side picked the generic
names; the http side made the more cautious choice. Cheap to align now.

### F18. `Params::values()` rebuilds the pointer table every call
**Performance, low.** `cache_dirty_` is invalidated on every `add*`. A
typical sequence is `add().add().add() → values()` once, so the rebuild fires
once. But `values()` is also called inside `PQsendQueryParams` once per query.
On reused/cached `Params`, the cache is stable. The implementation is fine
*now*, but the `mutable` cache is a code smell that disappears if `Params` is
made into a small-buffer-optimised value type (see §4).

### F19. `Lease` is wrapped in `shared_ptr`
**Ergonomics, low.** `Pool::acquire()` returns `Flow<shared_ptr<Lease>>`.
`Lease` is move-only by design. The only reason to `shared_ptr` it is so the
Flow callbacks can capture it. With Task/coroutine-first usage we can return
`Flow<Lease>` directly and let the coroutine frame own it.
**Evidence:** `db.cxx:602-630, 1113, 1128, 1166`.

### F20. No SQLite path
**Ergonomics, deferred.** The project's `coding_standards_db` skill mentions
sqlite3 patterns; the actual `conflux::db` is Postgres-only. If SQLite is
intended, the current API is not abstracted across backends — `PGconn`/`PGresult`
leak through `raw()` accessors. Decide explicitly: Postgres-only (rename
namespace `conflux::pg`) or build the abstraction now.

---

## 4. Proposed changes

Default stance: **breaking changes are fine** — the only callers in tree are
the bench and two examples. We update them in the same patch series.

### P1. Take SQL by `string_view`, by `shared_ptr<string const>`, or by stable handle (fixes F1) [revised 2026-04-28]

```cpp
// BREAKING — query/prepare/exec_prepared signatures change.

// Ergonomic overload: `string_view` is materialised into an owned `string`
// inside enqueue_job_ (the lambda capture must own the bytes — `run_query_`
// runs on the owner thread asynchronously, after the caller's stack frame
// has unwound).  Same allocation count as today; ergonomic-only win.
Flow<Result> Connection::query(std::string_view sql, Params params = {});

// Zero-copy overload: caller threads a cached SQL handle (e.g. from
// `QueryCache::lookup` / `load_async`).  No heap copy on the hot path; the
// lambda captures the `shared_ptr` instead of materialising a `string`.
Flow<Result> Connection::query(std::shared_ptr<std::string const> sql,
                               Params params = {});

Flow<void>   Connection::prepare(std::string_view name,
                                 std::string_view sql,
                                 std::span<Oid const> param_types = {});
Flow<Result> Connection::exec_prepared(std::string_view name, Params params = {});
```

**Ownership contract.** `enqueue_job_` defers SQL execution onto the
owner thread; the captured lambda must own the bytes the underlying
`PQsendQueryParams` reads. So:

- `string_view` overload: `run_query_` (rewritten) accepts
  `std::string` by value and the wrapper does
  `enqueue_job_([..., sql = string{sql}](){...})`. One owned copy
  per call. Same as today — F1's heap reduction is delivered by the
  `shared_ptr` overload, not by `string_view`.
- `shared_ptr<string const>` overload: lambda captures the
  `shared_ptr` by value (refcount bump only), `run_query_` reads
  `sql->c_str()`. Zero copies.

`QueryCache::load_async` returns a `shared_ptr<string const>` — that is
the canonical no-copy path. Hot-path callers should thread that handle
end-to-end.

Migration cost: trivial — `string` → `string_view` at call sites compiles
unchanged for all current callers. No heap saved if the caller passes a
`string` literal; saved when the caller threads the cache through.

### P2. SBO `Params`, binary mode, span-friendly bind (fixes F2, F3, F18)

```cpp
class Params {
    // Inline storage: kInline params stored without heap. Larger overflows
    // to a single owned arena (one allocation, monotonic bump).
    static constexpr size_t kInline = 8;
    // ... small_vector / arena layout

public:
    enum class Format : int { Text = 0, Binary = 1 };

    // Text path (back-compat).
    Params& add(std::string_view v);
    Params& add(std::int64_t v);                          // text
    Params& add(double v);                                // text
    Params& add(bool v);                                  // text
    Params& add_null();

    // Binary path.
    Params& add_binary(std::int64_t v, Oid oid = INT8OID);
    Params& add_binary(std::int32_t v, Oid oid = INT4OID);
    Params& add_binary(double v, Oid oid = FLOAT8OID);
    Params& add_binary(std::span<std::byte const> bytes, Oid oid = BYTEAOID);

    // Choose result format per query (default Text).
    Params& result_format(Format f) noexcept;
};
```

The arena buys: zero per-add `std::string`, no `mutable cache_dirty_`,
contiguous `char const*` table built once at `values()`. Binary mode buys
fewer text encodes/decodes and bytewise-stable doubles. `Oid` constants come
from `<libpq-fe.h>` server/catalog headers — we re-export the few we use as
`conflux::db::oids::*` rather than forcing callers to include `<server/catalog/pg_type_d.h>`.

Migration cost: existing call sites compile; `add(int64_t)` keeps the text
path. Opt-in `add_binary` for hot statements.

### P3. Prepared-statement registry and per-connection auto-prepare (fixes F4)

```cpp
class StatementCache {
public:
    struct Entry {
        std::string name;             // generated, stable across connections
        std::shared_ptr<std::string const> sql;
        std::vector<Oid> param_types;
    };

    // Stable name = "p_" + base32_no_pad(xxh3_64bits(sql)). Same SQL =
    // same name across connections, so the prepared name embedded into
    // queries is connection-independent. Width is fixed at 64 bits — never
    // truncate. Truncation creates silent SQL mis-execution under
    // collision; 64 bits gives ~2^32 statements before a 2^-32 collision
    // probability, which is well past any realistic working set.
    //
    // Output shape: "p_" + 13 base32 chars (64 bits → ceil(64/5) = 13).
    // No padding. ASCII identifier-safe. Stable across processes.
    static std::string stable_name(std::string_view sql);
};

class Connection {
public:
    // High-level: ensures the statement is prepared on this connection,
    // then executes it. First call on a fresh connection costs a Prepare;
    // subsequent calls go straight to PQsendQueryPrepared.
    Flow<Result> exec_cached(std::shared_ptr<StatementCache::Entry const> stmt,
                             Params params = {});
};
```

The cache lives on `Pool` (one per pool, not per connection). Each
`Connection` keeps a small `flat_set<std::string_view>` of names it has
already prepared this lifetime. Pool eviction (connection died) drops the
per-connection set with the connection.

Migration: low — `prepare/exec_prepared` stay; `exec_cached` is additive.
`QueryCache` and `StatementCache` tie together: callers pass the same
`shared_ptr<string const>` they got from `QueryCache::load()`.

### P4. Single-row streaming [revised 2026-04-28 — BLOCKED on framework primitive]

**Status: blocked.** The verification report correctly flagged that the
original P4 cited a `Stream<T>` that does not exist. We audited
`src/work/` end-to-end (`carrier_streams.cxx`, `carrier_coro.cxx`,
`carrier_model_a.cxx`, `carrier_model_b.cxx`, `carrier_scope.cxx`,
`carrier_deadline.cxx`, `carrier_timer.cxx`, `carrier_flags.cxx`,
`root.cxx`) and the inventory is:

- Single-shot results: `root::Task<T>`, `root::Posted<T>`,
  `root::Operation<T>` (and the matching `*JoinHandle`s, sources, and
  awaiters). One-value-per-handle, terminal `Outcome<T>`.
- Carriers: `model_a::Chain<T>`, `model_b::{Task,Posted,Operation}Chain<T>`,
  `model_a::EagerChain<T>`. Chains are also single-shot.
- Slots: `carrier::DroppableSlot<T>` (single-consumer, drains once),
  `carrier::CoalescingSlot<T>` (last-write-wins, `take()` consumes).
  Both are single-shot per handle.
- Cancellation/timing: `Scope`, `DeadlineScope`, `TimerService`,
  `LaneTimerScope`. Not data-flow primitives.

**There is no multi-shot stream / channel / queue primitive in
`conflux.work.*`.** Per human direction we do not invent one inside db.

Consequence: `query_stream(...)` cannot be expressed against current
`conflux.work` primitives and is **deferred** until a multi-shot
producer/consumer primitive exists at the framework level. F5 and the
F6-streaming-half remain open.

**Stop-gap (no API change):** F15's "drop `PGRES_SINGLE_TUPLE` from
`Result::ok()`" stays as proposed — once the streaming primitive lands,
`PGRES_SINGLE_TUPLE` is what re-enables it. Until then, big SELECTs are
materialised in libpq as today; document the memory profile in the API
docs and recommend `LIMIT`/`OFFSET` or `DECLARE CURSOR` workarounds.

```cpp
// (Future — not in this redesign.) Once a multi-shot primitive (working
// name `WorkStream<T>` or similar) exists in conflux.work, the API
// becomes:
//
//   WorkStream<Row> Connection::query_stream(std::string_view sql,
//                                            Params params = {},
//                                            QueryOptions opts = {});
//
// At that point P15 is reverted (PGRES_SINGLE_TUPLE re-accepted in
// Result::ok()) and PQsetSingleRowMode is called after PQsendQuery*.
```

Migration when unblocked: additive — `query()` keeps returning
`Flow<Result>` for the materialised case.

### P5. Transaction RAII helper with retry (fixes F7)

```cpp
struct TxOptions {
    enum class Iso { ReadCommitted, RepeatableRead, Serializable };
    Iso iso{Iso::ReadCommitted};
    bool read_only{false};
    bool deferrable{false};
    int  max_retries{3};
};

// All of these are coroutine-first.  `Body` must return `Task<R>` (or
// `Task<void>`); the helper unwraps the inner Task by `co_await`-ing it,
// so the helper's outer Task resolves to `R` (not `Task<R>`).
template <class Body>
    requires requires(Body b, Connection& c) {
        { b(c) } -> /* Task-like */;
    }
auto with_transaction(Connection& c, TxOptions opt, Body&& body)
    -> Task<typename detail::awaitable_value_t<std::invoke_result_t<Body, Connection&>>>;

// Equivalent expansion of the return type:
//   using BodyTask  = std::invoke_result_t<Body, Connection&>;        // Task<R>
//   using ResultT   = detail::awaitable_value_t<BodyTask>;            // R
//   return type     = Task<ResultT>
//
// where `awaitable_value_t<T>` is a small trait *defined locally* in
// `conflux.db.transaction` (it is not exported by `conflux.work`):
//
//   namespace conflux::db::detail {
//   template <class T> struct awaitable_value;
//   template <class T> struct awaitable_value<conflux::work::root::Task<T>> {
//       using type = T;
//   };
//   template <class T>
//   using awaitable_value_t = typename awaitable_value<T>::type;
//   }
//
// Inside the body, `with_transaction` does `auto r = co_await body(c);`
// so the unwrap happens at the call site, never producing Task<Task<R>>.

// Pool-level convenience: acquire + tx + release. Same unwrap rule.
template <class Body>
    requires requires(Body b, Connection& c) {
        { b(c) } -> /* Task-like */;
    }
auto with_transaction(Pool& p, TxOptions opt, Body&& body)
    -> Task<typename detail::awaitable_value_t<std::invoke_result_t<Body, Connection&>>>;
```

Behaviour: `BEGIN [ISOLATION LEVEL ...] [READ ONLY] [DEFERRABLE]`, run body,
`COMMIT`. On `PgError::is_serialization()` or `is_deadlock()`, `ROLLBACK` and
retry up to `max_retries` with capped exponential backoff. On any other
throw, `ROLLBACK` (best-effort, swallow secondary error) and rethrow.

Migration: additive. No existing code uses transactions yet.

### P6. `COPY ... FROM STDIN` / `TO STDOUT` (fixes F8) [revised 2026-04-28]

```cpp
class CopyIn {
public:
    Flow<void> write(std::span<std::byte const> chunk);   // PQputCopyData
    Flow<void> write_text(std::string_view line);         // appends '\n' if missing
    Flow<std::int64_t> finish();                          // PQputCopyEnd, returns rows affected
    Flow<void> abort(std::string_view reason);
};

// CopyOut polls one chunk at a time. Each next() resolves either with the
// next CopyData payload, or with std::nullopt to mark end-of-copy.
//
// Rationale (revised 2026-04-28): the original P6 used `Stream<span<byte>>`
// for CopyOut. There is no multi-shot stream primitive in conflux.work
// (see External dependencies / P4). A per-chunk `Flow<optional<vector<byte>>>`
// expresses the same shape using only primitives that exist today, and
// slots into the same single-flight queue as every other op.
//
// `next()` owns the buffer it returns: PQgetCopyData allocates, the wrapper
// wraps it in a vector with a custom deleter that calls PQfreemem.
class CopyOut {
public:
    Flow<std::optional<std::vector<std::byte>>> next();   // nullopt = end
    Flow<void> cancel();                                  // PQrequestCancel + drain
};

Flow<CopyIn>  Connection::copy_in (std::string_view sql);   // "COPY t FROM STDIN ..."
Flow<CopyOut> Connection::copy_out(std::string_view sql);   // "COPY t TO STDOUT ..."
```

The single-flight queue already serialises ops on the connection, so a
COPY-in session naturally locks the connection while the user streams chunks.
Caller iteration pattern: `while (auto chunk = co_await out.next()) { ... }`.

If/when a multi-shot stream primitive lands in `conflux.work` (see P4),
`CopyOut` may grow a stream-shaped overload; `next()` stays as the
primitive-light path.

Buffer ownership: `PQgetCopyData` allocates a `char*` the caller must free
with `PQfreemem`. The wrapper hides this — the returned vector owns its
buffer through a deleter that calls `PQfreemem`.

Migration: additive.

### P7. Pipeline mode (fixes F9)

```cpp
class Pipeline {
public:
    // Each call returns a Flow that resolves when this specific query's
    // result arrives. Internally, all sends share one flush; results are
    // demultiplexed in send order.
    Flow<Result> query(std::string_view sql, Params params = {});
    Flow<Result> exec_cached(std::shared_ptr<StatementCache::Entry const>, Params);

    // Forces a sync point on the wire. Caller normally lets RAII do this.
    Flow<void> sync();
};

Flow<Pipeline> Connection::pipeline();   // PQenterPipelineMode
```

Destructor of `Pipeline` runs `PQpipelineSync` + `PQexitPipelineMode`, drives
the consume loop until all in-flight Flows resolve, then releases the
connection from pipeline mode.

Migration: additive. Big perf win for write-heavy loops once it lands.

### P8. Pool: enforce `acquire_timeout`, run `on_acquire`, drop dead leases (fixes F10)

```cpp
struct PoolConfig {
    ConnectParams conn{};
    std::size_t min_connections{1};
    std::size_t max_connections{8};
    std::chrono::milliseconds acquire_timeout{std::chrono::seconds{5}};

    // Hook called on every newly-leased connection (e.g., re-prepare hot
    // statements, SET search_path, SET application_name).
    std::function<Task<void>(Connection&)> on_acquire{};

    // NEW: optional pre-return validation; if the hook throws, the
    // connection is closed instead of returned to the idle stack.
    std::function<Task<void>(Connection&)> on_release{};

    // NEW: idle-time expiry (libpq + server-side both age out).
    std::chrono::milliseconds max_idle_time{std::chrono::minutes{10}};
};
```

`acquire()` arms a `FileReader` timer (`file_io` already has `timeout_async`)
when enqueueing a waiter; on expiry the waiter is rejected with
`PgError{"acquire timeout", "..."}`. `on_acquire` is awaited before resolving
the lease.

Migration: low. Makes the existing fields actually do what they promise.

### P9. Return `Flow<Lease>` (fixes F19)

```cpp
// BREAKING.
Flow<Pool::Lease> Pool::acquire();
```

Coroutine usage becomes `auto lease = co_await pool->acquire();` instead of
`auto lease_sp = co_await pool->acquire(); auto& conn = **lease_sp;`. The
`shared_ptr<Lease>` indirection only existed to satisfy capture lifetime in
the old callback chain; not needed once Lease is `co_await`-aware.

Migration: trivial mechanical change in the two examples and the integration
test.

### P10. Async `QueryCache` with optional async loader (fixes F11)

```cpp
// Cache-hit path unchanged (synchronous, no IO).
std::shared_ptr<std::string const> QueryCache::lookup(std::string_view name) const noexcept;

// Cache miss: async by default if a FileReader is current, falls back to
// synchronous std::ifstream otherwise (matches existing behaviour for tests
// that don't mount a ring).
Flow<std::shared_ptr<std::string const>> QueryCache::load_async(std::string_view name);
```

Migration: the existing `load()` becomes `load_or_throw()` (sync path) and is
kept for tests/setup code; new `load_async()` is the one request handlers
use.

### P11. Pool-wide cancel pool, deadline-driven cancel (fixes F12, F14) [revised 2026-04-28]

```cpp
struct QueryOptions {
    std::optional<std::chrono::milliseconds> deadline{};
};

// Pool-wide: a single lazily-created `WorkPool{threads=1}` owned by `Pool`
// (or, for unpooled connections, a process-wide singleton accessed via
// `db::detail::cancel_pool()`).  PQcancel is blocking and infrequent — one
// thread per process is enough.  The Connection delegates cancel to this
// shared pool instead of owning its own thread.
Flow<void> Connection::cancel_inflight();

// New: arm an automatic cancel when the deadline fires.
Flow<Result> Connection::query(std::string_view sql,
                               Params params,
                               QueryOptions opts);
```

`opts.deadline`: arm a `FileReader` timer on send; on expiry, fire
`cancel_inflight()` and reject the original Flow with `PgError{"57014"}`.

Cancel pool ownership: created lazily on first `cancel_inflight()` call,
joined on `Pool` destruction (or at static-destructor time for the
process-wide fallback). Verified single-threaded so PQcancel calls
serialise; no per-Connection thread fan-out (with `max_connections=8`
the previous design would have spawned 8 cancel threads — rejected).

Migration: additive; existing `cancel_inflight(WorkPool&)` overload kept and
deprecated for one cycle.

### P11b. UTF-8 only `client_encoding` [added 2026-04-28, wiring revised 2026-04-28]

**db is UTF-8 only.** Multi-encoding support is out of scope. After the
connect dance reaches `PGRES_POLLING_OK` and the `Connection` is
constructed, the connection runs `SET client_encoding = 'UTF8'` once
through the existing async query machinery (no new primitive), then
verifies the server reports `UTF8` via
`PQparameterStatus(conn, "client_encoding")`. Mismatch is a fatal
connect-time error (`PgError{"22021", "client_encoding must be UTF8"}`).

**Wiring.** The connect dance runs on the ring lane; the `Connection`
constructor (`db.cxx:502`) records that lane's `thread::id` as
`owner_`. Because `connect()` is itself running on the owner thread, we
can call `query()` on the freshly-constructed connection without
violating the `owner_` check at `db.cxx:816-819`. We drive the SET
*before* resolving the outer connect Flow:

```cpp
// Inside detail::ConnectState::drive(), at the PGRES_POLLING_OK branch:
if (status == PGRES_POLLING_OK) {
    if (::PQsetnonblocking(conn.get(), 1) != 0) {
        dst.reject(make_exception_ptr(from_conn(conn.get(),
            "conflux.db: PQsetnonblocking")));
        return;
    }
    auto c = shared_ptr<Connection>(new Connection{move(conn), reader});

    // P11b: handshake the encoding before publishing the connection.
    // Reuses run_query_/drive_consume_loop_ — the same machinery every
    // user query goes through.  No new helper.
    auto outer   = dst;
    auto conn_sp = c;
    spawn(
        conn_sp->query(std::string_view{"SET client_encoding = 'UTF8'"})
        | then([outer, conn_sp](Result const&) mutable {
              auto* enc = ::PQparameterStatus(conn_sp->raw(),
                                              "client_encoding");
              if (enc == nullptr ||
                  std::string_view{enc} != std::string_view{"UTF8"}) {
                  outer.reject(std::make_exception_ptr(
                      PgError{"conflux.db: client_encoding must be UTF8",
                              "22021"}));
                  return;
              }
              outer.resolve(std::move(conn_sp));
          })
        | on_error([outer](std::exception_ptr const& ep) mutable {
              outer.reject(ep);
          })
        | on_cancel([outer]() mutable {
              outer.reject(std::make_exception_ptr(
                  PgError{"conflux.db: connect cancelled"}));
          }));
    return;
}
```

What this snippet relies on, and where each piece lives today:

- `Connection::query(string_view, Params)` — the P1 ergonomic overload
  added in this proposal. Materialises the SQL into a `string` inside
  `enqueue_job_`, so the `string_view` literal lifetime is fine.
  **Sequencing constraint:** P1 must land before P11b — the snippet
  will not compile otherwise.
- `flow | then(...) | on_error(...) | on_cancel(...)` + `spawn(...)`
  — the standard continuation pipeline from `conflux.work` (steps at
  `src/work.cxx:480-501`, pipe operators at `1340-1378`,
  `spawn(Flow<T>)` at `1394-1397`). Same idiom already used by
  `Pool::acquire` (`db.cxx:1120-1133`) so we are not inventing a new
  pattern. The `on_cancel` arm is required: without it, ring-teardown
  cancellation of the inner Flow would leave the public `connect()`
  Flow unresolved.
- `PQparameterStatus(conn, "client_encoding")` — defined for
  `client_encoding` since libpq 7.4 (we pin to 18, trivially
  satisfied). libpq updates the cached value when the server's
  `ParameterStatus` message arrives, which happens before
  `PQgetResult` returns its first row for the `SET` — so the value is
  fresh by the time we read it inside the `then` lambda.
- `Connection::raw() const noexcept` — already exists at `db.cxx:490`.
  Reuse it; do not add a separate `raw_handle()` accessor.

The query goes through `enqueue_job_` → `run_query_` → POLLOUT/POLLIN
loop → `drive_consume_loop_` → resolve. Because we are already on the
owner thread, `enqueue_job_` dispatches inline without crossing
threads. The handshake is a single-row `SET`, so `drive_consume_loop_`
sees one `PGRES_COMMAND_OK` then a `nullptr` and resolves once.

If the server fails the SET (very rare — only if the server build
genuinely lacks UTF8 support), the inner Flow rejects, the outer
`then(on_reject)` propagates, and the caller sees the underlying
`PgError`. Either way, the public `connect()` Flow resolves exactly
once — either with a UTF-8-verified `Connection` or with a
`PgError`.

`Row::as<std::string>` / `as<std::string_view>` are documented to return
UTF-8 bytes. No conversion layer, no encoding parameter on `ConnectParams`,
no per-query override. Resolves the verification report's charset gap.

Migration: callers cannot opt out. Servers configured with non-UTF-8
defaults must be re-set per-database (`ALTER DATABASE ... SET
client_encoding = 'UTF8'`) or rejected by db at connect.

### P12. Strongly-typed row binding (fixes F6 ergonomics)

```cpp
// optional<T> overloads.
template <class T> Row::as(int col) const -> T;
template <class T> Row::as_opt(int col) const -> std::optional<T>;

// Tuple-style structured binding.
template <class... Ts> std::tuple<Ts...> Row::as_tuple() const;

// User-defined mapping. Default reflects-on-aggregate via designated init.
template <class S> S Row::as_struct() const requires std::is_aggregate_v<S>;
```

`as_struct` uses the C++26 reflection facility (or, while we wait, a small
`describe<S>()` traits hook). For the immediate term, ship `as_opt<T>` and
`as_tuple<Ts...>`; defer reflection-based mapping until the compiler shipped
in CI supports it.

Migration: additive.

### P13. Module split (fixes F16)

```
src/db/
  db.cxx                    # umbrella: export import conflux.db.{types,...}
  types.cxx                 # PgError, Oid constants, RAII deleters
  params.cxx                # Params, binary helpers
  result.cxx                # Row, Result
  connection.cxx            # Connection, ConnectParams
  pool.cxx                  # Pool, Lease
  query_cache.cxx           # QueryCache
  pipeline.cxx              # Pipeline (P7)
  copy.cxx                  # CopyIn / CopyOut (P6)
  transaction.cxx           # with_transaction (P5)
```

Each is `export module conflux.db.<name>`. Umbrella `conflux.db`
re-exports them. Mirrors `conflux.net.http` umbrella pattern.

Migration: caller `import conflux.db;` continues to work. CMake gets per-file
parallelism on rebuilds.

### P14. Optional rename `conflux::db` → `conflux::pg` (addresses F17, F20)

If we commit to Postgres-only (and we should until SQLite is concretely
needed), rename to `conflux::pg`. Frees `conflux::db` to be the eventual
abstract surface. The rename is purely textual.

**Question for the user:** keep `conflux::db` and plan a SQLite/abstract
backend later, or rename to `conflux::pg` now? No new external dep either
way.

### P15. Drop `Result::ok() == PGRES_SINGLE_TUPLE` until P4 lands; revisit after (fixes F15)

Trivial. One line.

### P16. Cached column index helper (fixes F13)

```cpp
class Result {
public:
    // Stable handle for a column lookup; valid for the lifetime of *this.
    struct Column {
        int idx{-1};
        explicit operator bool() const noexcept { return idx >= 0; }
    };
    Column column(std::string_view name) const noexcept;  // computed once

    // Row::get(Column) is O(1).
};
```

Iteration becomes `auto c_id = r.column("id"); for (auto row : r) row.as<i64>(c_id);`.

Migration: additive; existing `Row::get(string_view)` stays.

---

## 5. Performance hypotheses (need benchmarks)

These are the claims the proposal makes that we should *measure*, not assert.
Add benchmarks to `benchmarks/db_*.cxx`:

1. **`Params` SBO + arena vs status quo.** New bench
   `db_params_bench.cxx`: bind 1/4/16/64 params of mixed types,
   1M iterations, no I/O. Hypothesis: ≥3× ns/iter improvement at param=4,
   ≥10× at param=16.

2. **Binary vs text format for INT8/FLOAT8.** Extend `db_coro_bench.cxx`
   with a `--binary` flag. Hypothesis: 10–25% ns/iter reduction at row=3
   (decode-bound), 20–40% at row=1000 (decode-bound and bandwidth slightly
   smaller).

3. **`PQsetSingleRowMode` + `Stream<Row>` vs full materialisation.** New
   bench `db_stream_bench.cxx` against `generate_series(1,N)` for
   N ∈ {1k, 100k, 10M}. Hypothesis: TTFB roughly constant in N (vs linear
   today); peak RSS roughly constant (vs O(N) today).

4. **Pipeline mode for write-heavy loops.** New bench `db_pipeline_bench.cxx`
   doing N small INSERTs with and without pipeline. Hypothesis: 5–20× ops/sec
   at N=100, gated by network RTT vs row count.

5. **`COPY FROM STDIN BINARY` vs `exec_prepared` loop.** New bench
   `db_copy_bench.cxx`. Hypothesis: ≥10× rows/sec for moderate-row payloads.

6. **Prepared-statement cache hit rate.** Extend `db_coro_bench.cxx` to run
   the same SQL through `query()` vs `exec_cached()`. Hypothesis: at row=3,
   `exec_cached` is 1.5–3× faster (skips planner).

7. **`shared_ptr<string const>` SQL vs `string` SQL.** Smallest, simplest
   bench. Hypothesis: 20–80 ns/iter saved on the bind path.

---

## 6. Out of scope / deferred

- **Async DNS resolution.** [revised 2026-04-28] Out of scope for db. This
  is a framework-level concern that will also serve the HTTP client and
  SMTP. db will not ship its own resolver, getaddrinfo-on-WorkPool shim,
  or DNS-specific classifier. Until the shared async DNS component
  exists, db documents that `host=` conninfo blocks the ring lane during
  `PQconnectStart` and recommends `hostaddr=` or pre-resolved IPs. See §1a.

- **Multi-shot streaming primitive (`Stream<T>` / `WorkStream<T>`).**
  [added 2026-04-28] Out of scope for db. P4 (single-row streaming) and
  the original P6 `CopyOut::rows()` shape both depend on it. db will not
  invent one; once `conflux.work` provides one, P4 unblocks and P6 grows
  a stream overload alongside its existing per-chunk `next()`.

- **SQLite backend.** Addressed only via P14 (rename question). Building a
  proper backend abstraction is a separate design — it'd want a `Backend`
  concept, a generic `Statement` type, and decoupled error taxonomy. Not now.

- **TLS to PostgreSQL via libpq.** libpq handles its own TLS through
  OpenSSL; there is no win in routing it through our io_uring TLS code.
  Document the conninfo `sslmode` knobs in the API doc; no code change.

- **Connection-aware load balancing / read replicas.** The `Pool` is single-
  endpoint by design. A `MultiPool` over named replicas is a follow-up.

- **`LISTEN`/`NOTIFY`.** Useful for cache invalidation and pubsub. The
  poll-based consume loop already supports the shape; add `Stream<Notification>`
  on `Connection::listen(channel)` in a follow-up.

- **`PQsendDescribe*` to validate column types compile-time vs server type
  Oid.** A genuine compile-time-checked layer would need either a build-step
  that introspects the schema or `consteval` SQL parsing — both are big
  projects. The runtime check on first execution is good enough until then.

- **Adopt libcurl.** Explicitly rejected. Project standard is no libcurl, no
  new external deps without sign-off. This proposal adds none.

- **Rewriting `Connection` to be lock-free across threads.** The single-thread
  pinning is a feature, not a bug — io_uring is per-thread, libpq is not
  thread-safe-per-connection, and the per-connection FIFO falls out naturally.
  Cross-thread queries are the wrong primitive; cross-thread *Pool* is what
  we want, and that's already a separate concern that lands cleanly on top
  of P3.

- **Async libpq via `io_uring_prep_recv` directly (skip libpq buffering).**
  Tempting for COPY OUT throughput, but reimplements the v3 protocol parser.
  Defer until a measurement shows libpq's buffer copies are the bottleneck.

---

## Verification Report

Adversarial read against `src/db/db.cxx` (1188 LOC, confirmed), tests/bench/examples,
and the `src/net/`, `src/file_io/`, `src/work/` reference idioms. Tone is blunt.
Every cite verified.

### Verdict per claim (Findings F1–F20)

| Finding | Verdict | Evidence |
|---|---|---|
| F1 SQL `string` by value forces alloc | ✓ confirmed | `db.cxx:472,474,476,807,847,887`; bench passes `string{kSql}` (`db_coro_bench.cxx:49,59`); examples pass literals (`db_pool.cxx:54,61`). Each call materialises a `std::string` from a literal/view. |
| F2 `Params` heap per `add` | ✓ confirmed | `db.cxx:399-407` — `add(string_view)` does `emplace_back(string{v})`; numeric paths route through it (`db.cxx:414-431`). 4 params + cache rebuild = ~5 small heap allocs. |
| F3 No binary format | ✓ confirmed | `formats_.push_back(0)` everywhere (`db.cxx:393,403,...`); `result_format()` hard-coded `0` (`db.cxx:451`); decode in `Row::as<T>` is `from_chars` text (`db.cxx:216-264`). |
| F4 No prepared-stmt cache | ✓ confirmed | `prepare()` and `exec_prepared()` take a name supplied by the caller (`db.cxx:474,476`). No registry, no auto-prepare on a fresh connection, no SQL→name interning. |
| F5 Last-result-only / no streaming | ✓ confirmed | `drive_consume_loop_` loops `PQgetResult` and overwrites `*partial` until null (`db.cxx:979-1008`). Comment at `db.cxx:964-968` admits "the loop silently keeps only the last result." `PQsetSingleRowMode` not referenced anywhere in the repo. |
| F6 `Row::as<T>` text reparse, no `optional<T>` | ✓ confirmed | Specialisations only for `string, string_view, i64, i32, double, bool` (`db.cxx:205-265`). No `optional<T>` overload anywhere. Tests use only those types (`db_test.cxx:236-271`). |
| F7 No transaction abstraction | ✓ confirmed | No `BEGIN`/`COMMIT`/`ROLLBACK` literals or helpers in `db.cxx`. Classifiers `is_serialization`/`is_deadlock` exist (`db.cxx:62-67`) but no retry loop uses them. |
| F8 No `COPY` | ✓ confirmed | Zero `PQputCopy*` / `PQgetCopyData` references in `src/`, `tests/`, `benchmarks/`. |
| F9 No pipeline | ✓ confirmed | Zero `PQenterPipelineMode`/`PQpipelineSync` references in repo. |
| F10 Pool dead config | ✓ confirmed | `acquire_timeout` declared `db.cxx:596`, set in test `db_integration_test.cxx:292`, **never read** in implementation (whole-repo grep verified). `on_acquire` declared `db.cxx:597`, **never read or set** anywhere. `acquire()` waiter path enqueues forever (`db.cxx:1136`); only `Pool::close()` (`db.cxx:1092-1094`) cancels waiters. |
| F11 `QueryCache` blocking I/O | ✓ confirmed | `ifstream in{path}` plus iterator copy (`db.cxx:571-579`). No async path. `file_io.cxx:675` has `open_async` and `read_into` etc. — could be used. |
| F12 `cancel_inflight(WorkPool&)` ergonomic burden | ✓ confirmed | `db.cxx:1044-1073` requires caller-owned pool; `db_integration_test.cxx:264` constructs one just to test cancel. |
| F13 `fnumber_sv_` 128-byte stack copy + linear search | ✓ confirmed | `db.cxx:128-139, 192-199, 303-308`. The `Result::column_index` cache is exposed but `Row::get(string_view)` does not consult it. |
| F14 No per-statement timeout | ✓ confirmed | Only `ConnectParams::connect_deadline` (`db.cxx:456`); `query/prepare/exec_prepared` take no `QueryOptions`. |
| F15 `PGRES_SINGLE_TUPLE` dead in `Result::ok()` | ✓ confirmed | `db.cxx:289-292`. Until single-row mode lands it cannot occur. |
| F16 Monolithic module | ✓ confirmed | `db.cxx:8` — single `export module conflux.db`. `net/` has 38 submodules under `conflux.net.*` (verified by grep). |
| F17 Naming collision with `conflux::http` | ⚠ partially right | `conflux::http::Connection`/`Pool` do **not** exist today (HttpClient is the only client class); collision is hypothetical, not actual. Proposal admits as much ("a future async HTTP client"). Speculative. |
| F18 `cache_dirty_` rebuild | ✓ confirmed but minor | `db.cxx:366-378, 445-447`. Rebuild fires once per query under typical use (one `values()` call inside `PQsendQueryParams`). Real cost is the `add()` heap path (F2). |
| F19 `Lease` wrapped in `shared_ptr` | ✓ confirmed | `Pool::acquire()` returns `Flow<shared_ptr<Lease>>` (`db.cxx:641, 1099`). `Lease` itself is move-only (`db.cxx:616-619`). |
| F20 No SQLite path | ✓ confirmed | Postgres-only. `raw()` accessors leak `PGconn`/`PGresult` (`db.cxx:282, 490`). |

### Verdict on the foundational async claim

✓ **Confirmed for the steady-state hot path.** `PQsendQueryParams`/`PQsendPrepare`/`PQsendQueryPrepared` then `PQflush` then `poll_add_oneshot(POLLOUT)` then `PQconsumeInput` then `while(!PQisBusy) PQgetResult` then `poll_add_oneshot(POLLIN)`. No `PQexec*` blocking call in the codebase (verified by repo-wide grep). `PQsetnonblocking(1)` is set right after `PGRES_POLLING_OK` (`db.cxx:707`).

⚠ **One unflagged blocking risk on connect.** `PQconnectStart` is documented non-blocking, but on libpq builds where `host=` (not `hostaddr=`) is given, **DNS resolution is performed synchronously inside `PQconnectStart`** before the first poll. The proposal doesn't flag this. For a hostname-based conninfo, a slow resolver blocks the ring lane during the connect dance. Same risk applies to GSSAPI/SSPI auth bring-up. Mitigation options: (a) document `hostaddr=` requirement, (b) pre-resolve via getaddrinfo on a `WorkPool`, (c) run `PQconnectStart` itself on a worker thread.

> **Resolution [2026-04-28]:** DNS is out of scope for db (see §1a /
> §6). The component will use the future framework-level async DNS
> resolver once it exists. Until then, db documents the `host=`
> blocking limitation and recommends `hostaddr=` or pre-resolved IPs.
> Mitigation (b) and (c) are *not* taken inside db.

⚠ **`PQfreemem` not called for `PQgetCopyData`.** Not in scope today (no COPY), but P6 must show buffer ownership; proposal omits it.

### Verdict per proposed change (P1–P16)

| Change | Verdict | Notes |
|---|---|---|
| P1 SQL by `string_view` / `shared_ptr<string const>` | ✓ sound, with one caveat | The `string_view` lifetime claim ("must outlive the flush") is correct because `run_query_` uses `sql.c_str()` synchronously inside the same call (`db.cxx:830-838`). However, `run_query_` is invoked via `enqueue_job_`, and the lambda *captures* the SQL — so the captured object must own the bytes. Proposal needs to copy `string_view` into the lambda or document the contract. As written, the snippet `query(std::string_view sql, Params)` would need an internal `string` capture or a small-string ownership wrapper. Not free. |
| P2 SBO `Params` + binary mode | ✓ sound | Compiles. Note: libpq's text params ignore `lengths_` for non-binary; today's lengths-population is wasted but harmless. Binary `add(double)` must serialise as IEEE 754 big-endian (libpq protocol = network order); proposal doesn't say. `Oid` re-export plan via `conflux::db::oids::*` is correct (`pg_type_d.h` lives in server include path, confirmed `/usr/include/postgresql-18/server/catalog/pg_type_d.h`). |
| P3 StatementCache + `exec_cached` | ✓ sound | Stable name via xxhash is fine. Missed: name-collision risk if **two different SQL strings** hash-collide (32-bit truncation of `xxhash`). Use full 64-bit and base32. Also missed: server-side `max_prepared_statements` quota — needs eviction on `42P05` (duplicate). |
| P4 `Stream<Row>` + `single_row_mode` | ✗ wrong premise (compiles, but) → **Resolved 2026-04-28: P4 marked BLOCKED** | Proposal claims "Conflux already has a stream primitive in `conflux.work` — reuse it." **There is no `Stream<T>` in conflux.work.** Repo-wide grep confirms zero matches. `conflux.work.carrier.streams` defines `DroppableSlot`/`CoalescingSlot` only — single-shot, not multi-shot streams. **Resolution per human direction (2026-04-28):** P4 is now explicitly marked blocked on a future framework primitive. db will *not* invent a streaming primitive locally. F5 and F6-streaming-half stay open until `conflux.work` ships a multi-shot producer/consumer type. F15 (`PGRES_SINGLE_TUPLE` from `Result::ok()`) stays as the only piece of P4 that lands now. |
| P5 `with_transaction` template | ⚠ signature is sloppy | `Body` returns a `Task<R>`, not `R`. The shown return type `Task<std::invoke_result_t<Body, Connection&>>` would be `Task<Task<R>>`. Needs the awaitable-unwrap pattern (e.g. `awaitable_traits<invoke_result_t<...>>::value_type`). Otherwise sound and right priority. |
| P6 `CopyIn`/`CopyOut` | ⚠ incomplete → **Partially resolved 2026-04-28** | API shape is right. Missing: `PQfreemem` call for `PQgetCopyData` buffers (libpq allocates), backpressure semantics for `PQputCopyData == 0` (which means "would block — try again"), and the protocol contract that COPY mode locks out non-COPY queries until done — needs explicit assertion in the FIFO queue. **Resolution (2026-04-28):** `CopyOut::rows()` originally returned a nonexistent `Stream<span<byte>>`; reshaped to `Flow<optional<vector<byte>>> next()` using only `conflux.work` primitives that exist today, with documented `PQfreemem` ownership in the wrapper. Backpressure and FIFO-lockout notes still apply at implementation time. Stream-shaped overload deferred until a multi-shot primitive lands. |
| P7 Pipeline mode | ✓ sound | Demux-by-send-order is correct (libpq guarantees this). Missing: behaviour on a query inside the pipeline that errors — libpq enters error-recovery state where subsequent results are `PGRES_PIPELINE_ABORTED` until the next sync. |
| P8 Pool: `acquire_timeout` etc. | ✓ sound | `file_io::timeout_async` exists (`file_io.cxx:1711`) so the implementation is straightforward. The new `on_release` and `max_idle_time` are additive scope creep but cheap. |
| P9 `Flow<Lease>` direct | ✓ sound | Trivial move-only flow; `FlowSource<T>` is copyable shared-state (`work.cxx:1404-1414`), so the carrier is fine. Two examples + one test to update. |
| P10 Async `QueryCache::load_async` | ✓ sound | `file_io::open_async` + `read_into` exist (`file_io.cxx:675, 792`). Proposal correctly preserves sync `load()` for non-ring callers. |
| P11 Connection-owned cancel pool + deadline | ⚠ subtle | The "lazy singleton WorkPool" inside `Connection` introduces a thread-per-connection at minimum, multiplied by the pool. Better: a *single* pool-wide cancel `WorkPool{threads=1}` lazily created on first `cancel_inflight` call. Proposal isn't explicit. Otherwise sound. |
| P12 `Row::as_opt` / `as_tuple` / `as_struct` | ✓ for `as_opt`/`as_tuple` | `as_struct` requires C++26 reflection; the project targets C++26 modules but the actual reflection facility ships piecemeal. Safe to defer as proposal already does. |
| P13 Module split | ✓ sound | Mirrors `conflux.net.*` (verified — 38 submodules). Build-graph parallelism real. |
| P14 `conflux::pg` rename | ⚠ speculative | F17 collision is hypothetical. Renaming has ABI/source churn cost across the future codebase. Defer until a SQLite backend is concretely planned. |
| P15 Drop `PGRES_SINGLE_TUPLE` from `ok()` | ✓ trivial | One line; safe. Will need to come back when P4 lands. |
| P16 `Result::Column` handle | ✓ sound | Already half-built — `Result::column_index(string_view)` exists (`db.cxx:303-308`), it's just not threaded through `Row::get(string_view)`. The proposal could be one-line tighter: have `Row::get(Column)` overload only. |

### Gaps the proposal missed

- **DNS resolution on connect blocks the ring lane** when conninfo uses `host=` instead of `hostaddr=`. `PQconnectStart` does the lookup synchronously. Same for GSSAPI auth init. (`db.cxx:760` — no pre-resolve.)

> **Resolution [2026-04-28]:** Reframed per human direction. Async DNS is
> a future framework component shared with HTTP client and SMTP — *not*
> a db responsibility. Until that component exists, db documents the
> `host=` blocking behaviour and recommends `hostaddr=` or pre-resolved
> IPs in the API doc. db will adopt the shared resolver when available.
> See §1a "External dependencies" and §6 "Out of scope".
- **`PQexec` vs `PQexecParams` consistency.** Codebase uses only the `PQsend*` async family (verified). Worth documenting that *no* sync path is allowed even for one-shot setup queries — to prevent regression. CLAUDE.md doesn't say it.
- **Charset / client_encoding handling.** `Connection::connect` doesn't set `client_encoding`; relies on server default. Tests assume UTF-8. A client receiving non-UTF-8 text columns through `Row::as<string_view>` will return raw bytes — proposal doesn't surface this.

> **Resolution [2026-04-28]:** db is UTF-8 only. See P11b. Connect issues
> `SET client_encoding = 'UTF8'` and verifies via
> `PQparameterStatus`; mismatch is a fatal connect error. No multi-encoding
> support, no per-query override.
- **Prepared-statement name collisions across pool connections.** Today there's no namespace; if caller uses `"q1"` on two connections in parallel, they're independent server-side names but the user must know. F4 partly covers but the *cross-connection lifetime* risk (a connection dies and the recycled one doesn't know about `"q1"`) isn't called out — only auto-prepare is.
- **libpq protocol version assumption.** `PQenterPipelineMode` requires libpq 14+ (P7). System libpq is 18.3 (`dev-db/postgresql-18.3`); pinning to system version (`libpq >= 18`) makes the constraint trivially satisfied. No feature gate needed. Proposal §1a updated 2026-04-28.
- **`PQsetnonblocking` failure path.** `db.cxx:707-710` rejects on failure but doesn't close the half-connected `PGconn`. Minor leak risk; proposal silent.
- **`PQconsumeInput` never returns 0 spuriously, but if the kernel returns EAGAIN on the underlying `recv` mid-byte, the loop re-arms POLLIN — fine — but no guard against an infinite POLLIN→consume→POLLIN loop if the server keeps trickling.** Not a real bug but proposal didn't audit.
- **Pool: connect failure during `acquire()` decrements `total_` but does not retry or dispatch waiting acquirers.** `db.cxx:1130-1133`. If `min_connections` is 0 and the only growth attempt fails, subsequent acquires won't try again until idle becomes non-empty (which it won't). P8 covers timeouts but not retry policy.
- **Pool: there is no health check on idle reuse.** `Pool::return_` checks `conn->ok()`, but a connection idle for 10 minutes may have been killed by `tcp_keepalive` server-side; next use will fail mid-`PQflush`. Proposal mentions `max_idle_time` but no `SELECT 1` ping.
- **Pool: no listen-side validation that a `Connection` returned by a leased Lease's destructor is still on the same FileReader/ring as the one currently active.** `reader_` is captured at construct time (`db.cxx:498-502`); if the Pool is shared across rings (currently disallowed by `owner_` check) the reader pointer would be stale. The single-thread invariant is enforced but not encoded in types.
- **`bool Row::as<bool>`** accepts `"1"`/`"0"` but Postgres serialises booleans as `"t"`/`"f"`. The `"1"`/`"0"` codepath is dead in real use; not a bug, but a smell.
- **`Params::add` lacks `uint64_t` / `size_t` overloads.** `add(size_t{42})` is ambiguous between `add(i64)` and `add(string_view)` (since `size_t` has integer promotions). Proposal P2 doesn't add it either.
- **`fnumber_sv_` rejects names ≥128 chars.** Postgres `NAMEDATALEN` defaults to 64, so 128 is safe. But Postgres can be compiled with larger `NAMEDATALEN`; the magic 128 in `db.cxx:131` is a quiet cap. Either lift to `dynamic` or document.
- **No metrics hooks.** `src/net/metrics.cxx` exposes a metrics module; `conflux::db` has zero hooks for query count, latency, error rates. Net side has them. Parity gap.
- **No structured logging.** Same — `src/net/structured_log.cxx` exists; db has none.
- **`Result::ok()` accepts `PGRES_COMMAND_OK` for `INSERT/UPDATE/DELETE` but `Result::rows()` then returns 0.** Caller-visible: a successful `INSERT` resolves the Flow with a `Result` whose `rows() == 0` and `command_tag() == "INSERT 0 1"`. Discoverability is poor; an `affected_rows()` helper that parses the tag would be a small additive win (proposal P12 doesn't include).
- **No `PQescapeIdentifier`/`PQescapeStringConn` helpers.** Callers that need dynamic table/column names today have no first-class tool; they roll their own. Worth adding.
- **No NOTICE handler.** libpq's default writes notices to stderr. `PQsetNoticeReceiver`/`PQsetNoticeProcessor` not used. Proposal silent.
- **`PQconnectStart` failure does not surface DNS error specifically.** Generic `from_conn` wraps it as a `PgError` with sqlstate `08001` if status is `CONNECTION_BAD`. Fine, but no separate classifier `PgError::is_dns_failure()`.

### Recommended priority ordering (independent of proposal)

Triaged by **(impact × confidence) / (cost × risk)**.

1. **P1 + P15 + P11b** — `string_view` SQL, drop `PGRES_SINGLE_TUPLE` from `ok()`, enforce UTF-8 `client_encoding` on connect. Trivial mechanical changes, broad win, zero ambiguity. Do first. [P11b added 2026-04-28]
2. **P10** — async `QueryCache::load_async`. Small, confined, removes a real foot-gun for request-handler code. Cheap.
3. **P8** — Pool: enforce `acquire_timeout`, run `on_acquire`, add idle health check. Currently the dead-config is misleading and waiters can hang. Bug-fix flavour.
4. **P9** — `Flow<Lease>` direct. Trivial; better ergonomics for the imminent coroutine usage.
5. **P5** — `with_transaction`. High user-facing value; small surface area; standalone module. Fix the `Task<Task<R>>` signature first.
6. **P12 (partial)** — `as_opt<T>` and `as_tuple<Ts...>`. High ergonomic value, no infra needed. Defer `as_struct` until reflection.
7. **P11** — connection/pool-owned cancel pool + deadline. Wraps F12 + F14. Plumb after P5 and P9.
8. **P2** — SBO `Params` + binary mode. Highest *measured* perf upside, but requires bench discipline (see §5) to avoid premature optimisation. Bench first.
9. **P3** — StatementCache + `exec_cached`. Real win; non-trivial cross-connection bookkeeping. Land after P2 so binary mode is available for prepared statements.
10. **P13** — module split. Pure refactor; do during a quiet window, not while actively iterating on P1–P3.
11. **P16** — `Result::Column`. Cheap; do alongside P12.
12. **P7** — pipeline mode. Big win for write-heavy paths; non-trivial demux logic. Land after P2/P3 are stable.
13. **P6** — COPY. Big win for any data-loading consumer, but no in-tree consumer today; only land when one shows up. Spec-only until then.
14. **P4** — Stream<Row>. **Blocked on a multi-shot stream primitive in `conflux.work`** (resolved direction 2026-04-28: db will *not* invent one locally; see P4). Only F15 (drop `PGRES_SINGLE_TUPLE` from `Result::ok()`) lands now; the rest waits for the framework primitive.
15. **P14** — `conflux::pg` rename. Defer until a SQLite backend is concretely planned. Pure churn otherwise.

### Open questions for the human

1. **Namespace.** Commit to `conflux::pg` now, or stay `conflux::db` and plan a backend abstraction later? Cost is mostly textual; the reason to decide now is that P3/P5/P6 will all use the namespace prefix in headers/skills and renaming after they land is N× more churn.
2. **`Stream<T>` in `conflux.work`.** P4 assumes it exists. It doesn't. Are we (a) inventing it as part of this work, (b) using a `Flow<optional<Row>>` chain instead, or (c) deferring P4 entirely?
   > **Answered [2026-04-28]:** (c). P4 is blocked on a future framework primitive. db will not invent a streaming type. P6's `CopyOut` was reshaped to `Flow<optional<vector<byte>>> next()` (option (b)-flavour, per-chunk) so the COPY path is unblocked.
3. **DNS on connect.** Acceptable to require `hostaddr=` in conninfo (force the caller to pre-resolve), or do we need a `getaddrinfo`-on-`WorkPool` shim before `PQconnectStart`?
   > **Answered [2026-04-28]:** Neither inside db. Async DNS is a separate framework component (shared with HTTP client and SMTP). Until it lands, document `hostaddr=` / pre-resolve as the recommended path; no shim ships in db.
4. **libpq min version.** ~~Pin to libpq ≥ 14 (enables P7 pipeline) or stay broad and feature-gate?~~
   > **Answered [2026-04-28]:** Pin to system libpq ≥ 18 (`dev-db/postgresql-18.3`). All P7+ features trivially available. No feature gates.
5. **Prepared-statement quota.** Default Postgres has no hard cap on prepared statements per session, but a long-lived pool connection that auto-prepares unbounded distinct SQL strings will leak. Add an LRU eviction in P3?
6. **Metrics / structured-log parity with `src/net/`.** In scope for this redesign, or separate ticket?
7. **`Connection::raw()` / `Result::raw()` exposure.** Keep (preserves escape hatch for future libpq features) or remove (cleaner API, blocks SQLite abstraction)?
8. **Cross-thread Pool.** Stay single-thread, or add a thin `MultiPool` over per-thread pools? Not in scope here but ergonomically a large delta.

### Summary

Proposal's foundational async claim is **right** in steady state but understates DNS/auth-time risk. The hot-path allocation findings are **all confirmed** with file:line evidence. Dead-code claims (`acquire_timeout`, `on_acquire`) are **confirmed**; tests even *set* `acquire_timeout` and never assert it. The biggest factual error is P4 asserting "`Stream<T>` already exists in `conflux.work`" — it does not. Several proposed snippets need signature fixes (P5 `Task<Task<R>>`, P1 `string_view` lifetime). Naming/rename items (F17/P14) are speculative. Missing items: metrics, structured logging, NOTICE handler, charset, identifier escapers, idle health check, pool retry policy on connect failure.

### Resolution log [2026-04-28]

Three constraints from the human, applied in this revision:

1. **Async DNS** is out of scope for db. New §1a "External dependencies"
   names the future framework-level component as a dependency. §6 "Out of
   scope" lists DNS explicitly. The Verification Report DNS gap is
   resolved by reframing — db documents `host=` blocking and recommends
   `hostaddr=` / pre-resolved IPs until the shared async DNS exists.
2. **`client_encoding` = UTF-8 only** (no flexibility). New P11b enforces
   `SET client_encoding = 'UTF8'` on connect plus a `PQparameterStatus`
   verification, with a fatal connect error on mismatch. Resolves the
   charset gap.
3. **`conflux.work` primitive audit.** Inventory of `src/work/` confirms
   no multi-shot stream/channel/queue exists. Single-shot primitives
   (`root::Task<T>`, `Posted<T>`, `Operation<T>`, carrier `Chain<T>`,
   `DroppableSlot<T>`, `CoalescingSlot<T>`) are all single-consumer,
   single-value. Consequences:
   - **P4 is BLOCKED.** Reworked to call this out explicitly. db will
     not invent a streaming primitive locally.
   - **P6 `CopyOut::rows()` reshaped** from `Stream<span<byte>>` to
     `Flow<optional<vector<byte>>> next()` — expressible with existing
     primitives (`Flow<T>` from legacy `conflux.work`). Stream-shaped
     overload deferred until a multi-shot primitive lands.
   - All other proposed signatures rely on `Flow<T>` / `FlowSource<T>`
     (legacy `conflux.work`) and/or single-shot carrier types, all of
     which exist today; no further proposal items needed reshaping.
