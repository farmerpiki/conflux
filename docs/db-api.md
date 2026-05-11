# conflux DB API Reference

**Module:** `conflux.db`  
**Namespace:** `conflux::db`  
**Backend:** libpq (PostgreSQL)  
**CMake feature gate:** `CONFLUX_HAS_DB`

See also: `examples/db_basic.cxx`, `examples/db_pool.cxx`.

---

## Build

```cmake
target_link_libraries(mytarget PRIVATE conflux::db)
```

Requires libpq. Configure must find `libpq` or the build fails — no silent degradation.

---

## Connection

### `ConnectParams`

```cpp
struct ConnectParams {
    std::string                    conninfo;          // libpq connection string or DSN
    std::chrono::milliseconds      connect_deadline;  // max time to establish connection
};
```

### `Connection`

```cpp
class Connection {
public:
    static expected<Connection, DbError> connect(ConnectParams const&);

    // Single query — returns result directly
    expected<Result, DbError> query(std::string_view sql);
    expected<Result, DbError> query(std::string_view sql, QueryOptions const&);

    // Prepared statement — prepare then execute in two calls
    expected<void,   DbError> prepare  (std::string_view name, std::string_view sql);
    expected<void,   DbError> prepare  (std::string_view name, std::string_view sql, std::span<Oid const> param_types);
    expected<Result, DbError> exec_prepared(std::string_view name, /* params... */);

    // Statement cache — prepare-once, execute-many via stable name
    expected<Result, DbError> exec_cached(std::string_view sql, /* params... */);

    // Pipeline — batch multiple queries, flush once
    Pipeline pipeline();

    // Misc
    bool        ok()            const noexcept;
    std::string last_error()    const;
    PGconn*     raw()           noexcept;          // escape hatch
    int         backend_pid()   const;
    int         server_version() const;
    void        cancel_inflight();
    void        close();
};
```

`connect` is blocking. For async-compatible connection use a thread pool or pre-connect before entering the io_uring loop.

### `QueryOptions`

```cpp
struct QueryOptions {
    std::chrono::milliseconds deadline{};  // 0 = no deadline
};
```

---

## Statement cache (`StatementCache`)

Transparently caches prepared statements by SQL text. `exec_cached` hashes the SQL, prepares once per connection lifetime, and re-executes on subsequent calls.

```cpp
// Accessed via Connection::exec_cached — not usually used directly
struct StatementCache {
    struct Entry {
        std::string              name;
        std::shared_ptr<std::string const> sql;
        std::vector<Oid>         param_types;
    };

    expected<Entry const*, DbError> get(std::string_view sql);
    expected<Entry const*, DbError> get(std::string_view sql, std::span<Oid const> param_types);
    static std::string stable_name(std::string_view sql);  // deterministic name from SQL hash
    void clear();
};
```

---

## Pipeline

Batches multiple queries and executes them in order during `sync()`. Current implementation is a logical batching barrier (executes queued items sequentially through `Connection::query` machinery); true libpq wire-level pipeline mode (`PQenterPipelineMode` / `PQpipelineSync`) is a follow-up.

```cpp
class Pipeline {
public:
    root::Task<Result> query(std::string_view sql, Params params = {});
    root::Task<Result> exec_cached(std::shared_ptr<StatementCache::Entry const>, Params);
    root::Task<void>   sync();
};

// Obtained from Connection:
Flow<Pipeline> Connection::pipeline();
```

Contracts:
- Owner-thread only (same lane/thread as `Connection`); one active pipeline per connection.
- `query()` rejected while `sync()` is in progress.
- `sync()` drains queued work in-order, resolves/rejects each flow exactly once.
- Destructor rejects unresolved queued results as `pipeline closed`.
- `Pool::acquire` returns a `Lease`; caller constructs `Pipeline` from `*lease`.

---

## Result / Row API

```cpp
class Result {
public:
    bool        ok()          const noexcept;
    std::string error()       const;
    size_t      row_count()   const noexcept;
    size_t      col_count()   const noexcept;
    std::string col_name(size_t col) const;

    Row         row(size_t i) const;
    // range-for: yields Row
    auto begin() const;
    auto end()   const;
};

class Row {
public:
    bool             is_null  (size_t col) const noexcept;
    std::string_view get_text (size_t col) const;           // text-format value
    // typed accessors via get<T>(col):
    template<class T> expected<T, DbError> get(size_t col) const;
};
```

Built-in `get<T>` specializations: `int32_t`, `int64_t`, `double`, `bool`, `std::string`, `std::string_view` (borrows from result), `std::chrono::system_clock::time_point`.

---

## Connection pool

### `PoolConfig`

```cpp
struct PoolConfig {
    ConnectParams                              conn;
    size_t                                     min_connections{1};
    size_t                                     max_connections{8};
    std::chrono::milliseconds                  acquire_timeout{5'000};
    std::function<root::Task<void>(Connection&)> on_acquire{};  // called after each acquire
};
```

### `Pool` and `Lease`

```cpp
class Lease {
public:
    Connection& operator*()  noexcept;
    Connection* operator->() noexcept;
    explicit operator bool() const noexcept;
    // destructor returns connection to pool
};

class Pool {
public:
    explicit Pool(PoolConfig);
    root::Task<Lease> acquire();  // suspends until a connection is available or timeout
};
```

`Lease` is move-only. The destructor returns the connection to the pool. Do not use the connection after the `Lease` is destroyed.

`acquire()` blocks (suspends the coroutine) until a connection is free or `acquire_timeout` elapses. On timeout, returns an error lease (`operator bool()` is false).

### `TxOptions`

```cpp
struct TxOptions {
    std::string isolation_level{"READ COMMITTED"};
    bool        read_only{false};
};
```

### Transaction helpers

```cpp
// With an existing connection
template<class Body>
root::Task<...> with_transaction(Connection& c, TxOptions opt, Body&& body);

// Acquires from pool, runs body in a transaction
template<class Body>
root::Task<...> with_transaction(Pool& p, TxOptions opt, Body&& body);
```

`body` is a coroutine lambda that receives a `Connection&`. On success it must return the result value. On any exception or co_return of an error the helper rolls back automatically; on success it commits.

```cpp
auto result = co_await with_transaction(pool, {},
    [](Connection& c) -> root::Task<int64_t> {
        auto r = co_await c.query("INSERT INTO t (v) VALUES (1) RETURNING id");
        co_return (*r).row(0).get<int64_t>(0).value();
    });
```

---

## Error

```cpp
struct DbError {
    std::string message;
    std::string sqlstate;   // 5-char SQLSTATE code (empty for client-side errors)
    int         os_errno{0};
};
```
