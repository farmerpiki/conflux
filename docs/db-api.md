# Conflux DB API Reference

- **Primary module:** `conflux.pg`
- **Primary namespace:** `conflux::pg`
- **Backend:** libpq (PostgreSQL)
- **Configure/build gates:** `CONFLUX_BUILD_DB_POSTGRES` and `CONFLUX_POSTGRES_PROVIDER`
- **Compiled feature macro:** `CONFLUX_HAS_DB`

See also: `examples/advanced/db_basic.cxx`, `examples/advanced/db_pool.cxx`.

---

## Build

```cmake
target_link_libraries(mytarget PRIVATE conflux::pg)
```

Requires libpq. Configure must find `libpq`, `CONFLUX_POSTGRES_PROVIDER` must be `AUTO` or `LIBPQ`, and `CONFLUX_BUILD_DB_POSTGRES` must resolve enabled or the DB component is unavailable. When compiled, targets that link the DB component receive `CONFLUX_HAS_DB=1`.

Use `conflux.pg` / `conflux::pg` for public PostgreSQL code. The older
`conflux.db` implementation spelling may remain internally while rename churn is
evaluated, but it is not the advertised preview API.

---

## Connection

### `ConnectParams`

```cpp
struct ConnectParams {
    std::string               conninfo{};                         // libpq connection string or DSN
    std::chrono::milliseconds connect_deadline{std::chrono::seconds{15}};
};
```

### `QueryOptions`

```cpp
struct QueryOptions {
    std::optional<std::chrono::milliseconds> deadline{};
};
```

`deadline == nullopt` or `deadline <= 0` means no per-query deadline.

### `Connection`

```cpp
class Connection : public std::enable_shared_from_this<Connection> {
public:
    static root::Task<std::shared_ptr<Connection>> connect(ConnectParams const&);

    root::Task<Result> query(std::string_view sql, Params params = {});
    root::Task<Result> query(std::shared_ptr<std::string const> sql, Params params = {});
    root::Task<Result> query(std::string_view sql, Params params, QueryOptions opts);

    root::Task<void> prepare(std::string_view name, std::string_view sql,
                             std::span<Oid const> param_types = {});
    root::Task<void> prepare(std::string_view name, std::shared_ptr<std::string const> sql,
                             std::span<Oid const> param_types = {});
    root::Task<Result> exec_prepared(std::string_view name, Params params = {});
    root::Task<Result> exec_cached(std::shared_ptr<StatementCache::Entry const> const& stmt,
                                   Params params = {});

    root::Task<void> cancel_inflight(WorkPool& cancel_pool);
    root::Task<void> cancel_inflight();
    root::Task<Pipeline> pipeline();

    bool        ok() const noexcept;
    std::string last_error() const;
    PGconn*     raw() const noexcept;
    int         backend_pid() const noexcept;
    int         server_version() const noexcept;
    void        close() noexcept;
};
```

`connect` and all query/prepare operations are coroutine tasks. They must run with a current `FileReader` on the owning ring lane. DB failures complete the returned task with a `PgError` exception; they do not return `std::expected`.

---

## Parameters (`Params`)

```cpp
namespace conflux::pg::oids {
inline constexpr Oid bool_ = 16;
inline constexpr Oid bytea = 17;
inline constexpr Oid int8  = 20;
inline constexpr Oid int4  = 23;
inline constexpr Oid text  = 25;
inline constexpr Oid float8 = 701;
}

class Params {
public:
    Params& add_null();
    Params& add(std::string_view);
    Params& add(char const*);
    Params& add(i64);
    Params& add(i32);
    Params& add(double);
    Params& add(bool);
    Params& add_json(std::string_view);

    Params& add_binary(i64, Oid oid = oids::int8);
    Params& add_binary(i32, Oid oid = oids::int4);
    Params& add_binary(double, Oid oid = oids::float8);
    Params& add_binary(std::span<std::byte const>, Oid oid = oids::bytea);

    Params& result_format(int fmt) noexcept;
    int count() const noexcept;
    int result_format() const noexcept;
};
```

Text parameters are stored in an internal arena and exposed to libpq as stable pointers for the lifetime of the `Params` object. Up to eight parameters use inline metadata before spilling to vectors.

---

## Statement cache (`StatementCache`)

Caches prepared statements by deterministic statement name derived from SQL text.

```cpp
class StatementCache {
public:
    struct Entry {
        std::string name;
        std::shared_ptr<std::string const> sql;
        std::vector<Oid> param_types;
    };

    static std::string stable_name(std::string_view sql);

    std::shared_ptr<Entry const> get(std::shared_ptr<std::string const> sql,
                                     std::vector<Oid> param_types = {});
    std::shared_ptr<Entry const> get(std::string_view sql,
                                     std::vector<Oid> param_types = {});
    void clear() noexcept;
};
```

Pass an entry to `Connection::exec_cached(...)` or `Pipeline::exec_cached(...)`. The connection tracks prepared names and prepares once per connection lifetime.

---

## Query file cache (`QueryCache`)

Loads `*.psql` files from a configured directory and caches the file contents as shared SQL strings.

```cpp
class QueryCache {
public:
    explicit QueryCache(std::filesystem::path root);

    std::shared_ptr<std::string const> lookup(std::string_view name) const noexcept;
    std::shared_ptr<std::string const> load_or_throw(std::string_view name) const;
    root::Task<std::shared_ptr<std::string const>> load_async(std::string_view name);
    void clear() noexcept;
};
```

Query names must be simple file stems: non-empty, no slash or backslash, no leading dot, and no `..`. `load_or_throw("select_one")` reads `<root>/select_one.psql`; missing files raise `std::filesystem::filesystem_error`, invalid names raise `std::invalid_argument`.

---

## Pipeline

Batches multiple queries and sends them through libpq wire-level pipeline mode during `sync()` (`PQenterPipelineMode` / `PQpipelineSync`). Results are demultiplexed in wire order back to the tasks returned by `query()` / `exec_cached()`.

```cpp
class Pipeline {
public:
    root::Task<Result> query(std::string_view sql, Params params = {});
    root::Task<Result> exec_cached(std::shared_ptr<StatementCache::Entry const> const& stmt,
                                   Params params = {});
    root::Task<void> sync();
};

// Obtained from Connection:
root::Task<Pipeline> Connection::pipeline();
```

Contracts:
- Owner-thread only (same lane/thread as `Connection`); one active pipeline per connection.
- `query()` and `exec_cached()` are rejected while `sync()` is in progress.
- `sync()` enters libpq pipeline mode, sends queued work without per-query round trips, then drains results until `PGRES_PIPELINE_SYNC`.
- `exec_cached()` sends a `PQsendPrepare` message before the first pipelined `PQsendQueryPrepared` for a statement name that is not yet prepared on the connection.
- Results are resolved/rejected in wire order; a server-side pipeline abort rejects the affected query tasks.
- Destructor rejects unresolved queued results as `pipeline closed`; active wire drains are allowed to finish enough to release libpq pipeline mode.
- A pool caller first acquires a `Lease`, then constructs a pipeline from the leased connection (`co_await lease->pipeline()`).

---

## Result / Row API

```cpp
class Result {
public:
    PGresult* raw() const noexcept;
    explicit operator bool() const noexcept;
    ExecStatusType status() const noexcept;
    bool ok() const noexcept;
    int rows() const noexcept;
    int cols() const noexcept;
    std::string_view column_name(int col) const noexcept;
    int column_index(std::string_view name) const noexcept;
    Column column(std::string_view name) const noexcept;
    std::string_view command_tag() const noexcept;

    Row operator[](int row) const noexcept;
    auto begin() const noexcept;
    auto end() const noexcept;
};

struct Column {
    int idx{-1};
    explicit operator bool() const noexcept;
};

class Row {
public:
    int ncols() const noexcept;
    bool is_null(int col) const noexcept;
    bool is_null(Column col) const noexcept;
    std::string_view get(int col) const noexcept;
    std::string_view get(Column col) const noexcept;
    std::string_view get(std::string_view col) const;    // throws PgError if missing
    int length(int col) const noexcept;

    template<class T> T as(int col) const;
    template<class T> T as(Column col) const;
    template<class T> std::optional<T> as_opt(int col) const;
    template<class T> std::optional<T> as_opt(Column col) const;
    template<class... Ts> std::tuple<Ts...> as_tuple(int start = 0) const;
};
```

Built-in `Row::as<T>` specializations: `i32`, `i64`, `double`, `bool`, `std::string`, `std::string_view` (borrows from the result). Parse failures throw `PgError`.

---

## Connection pool

### `PoolConfig`

```cpp
struct PoolConfig {
    ConnectParams                              conn{};
    size_t                                     min_connections{1};
    size_t                                     max_connections{8};
    std::chrono::milliseconds                  acquire_timeout{std::chrono::seconds{5}};
    std::function<root::Task<void>(Connection&)> on_acquire{};
};
```

### `Pool` and `Lease`

```cpp
class Pool : public std::enable_shared_from_this<Pool> {
public:
    class Lease {
    public:
        Connection& operator*() const noexcept;
        Connection* operator->() const noexcept;
        explicit operator bool() const noexcept;
    };

    static std::shared_ptr<Pool> create(PoolConfig cfg);
    root::Task<Lease> acquire();
    void close() noexcept;
    size_t total() const noexcept;
    size_t idle() const noexcept;
};
```

`Lease` is move-only. The destructor returns the connection to the pool; do not use the connection after the `Lease` is destroyed.

`acquire()` suspends until a connection is available or `acquire_timeout` elapses. Pool closed, off-owner acquire, connection failure, and acquire timeout complete the task with a `PgError` exception. External cancellation of a queued acquire completes the task as cancelled. A successful acquire returns a truthy `Lease`.

---

## Transactions

### `TxOptions`

```cpp
struct TxOptions {
    enum class Iso : std::uint8_t {
        ReadCommitted,
        RepeatableRead,
        Serializable,
    };

    Iso  iso{Iso::ReadCommitted};
    bool read_only{false};
    bool deferrable{false};
    int  max_retries{3};
};
```

### Helpers

```cpp
template<class Body>
root::Task<...> with_transaction(Connection& c, TxOptions opt, Body&& body);

template<class Body>
root::Task<...> with_transaction(Pool& p, TxOptions opt, Body&& body);
```

`body` is a coroutine lambda that receives a `Connection&`. On success it commits and returns the body value. On exception it rolls back and rethrows. `PgError` values with serialization (`40001`) or deadlock (`40P01`) SQLSTATEs retry until `max_retries` is exhausted.

```cpp
auto id = co_await with_transaction(pool, {},
    [](Connection& c) -> root::Task<int64_t> {
        auto r = co_await c.query("INSERT INTO t (v) VALUES (1) RETURNING id");
        co_return r[0].as<int64_t>(0);
    });
```

---

## Error

DB operations report failures by completing the returned `root::Task` with `PgError`.

```cpp
struct PgError final : std::runtime_error {
    std::string    sqlstate{};
    std::string    detail{};
    std::string    hint{};
    std::string    where{};
    ExecStatusType status{PGRES_FATAL_ERROR};

    bool is_unique_violation() const noexcept; // 23505
    bool is_serialization() const noexcept;    // 40001
    bool is_deadlock() const noexcept;         // 40P01
    bool is_connection_lost() const noexcept;  // SQLSTATE class 08
};
```

Client-side errors also use `PgError`; SQLSTATE may be empty for local validation or scheduling failures.
