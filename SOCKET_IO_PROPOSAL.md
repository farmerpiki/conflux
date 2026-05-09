# socket_io — io_uring socket abstraction

Module `conflux.socket_io`. Peer to `conflux.file_io`.
Single TU initially: `src/socket_io/socket_io.cxx`.

## Status

**Raw layer landed** — `SocketRawRing`, `BufferRing`, `DirectFdTable`, `GenerationTable`, all raw submission helpers. Builds clean on Clang + GCC, 404 tests pass.

**Revised per review** — split `SocketRawRing` (explicit user_data, no CompletionTable) from future `SocketTaskRing` (coroutine API). Raw APIs use `_borrowed` suffix for lifetime contracts. `RecvBuffer` has both `release()` (recycle now) and `detach()` (skip recycle). AF_ALG deferred.

## Problem

Socket io_uring work is scattered and duplicated:
- **http_server.cxx** — multishot accept, multishot recv + buffer ring, send, linked shutdown→close, direct fds, async setsockopt, generation counters, batch CQE dispatch. All hand-rolled, ~500 lines of io_uring plumbing.
- **dns.cxx** — per-query temp io_uring ring, recvmsg, sendto. UDP-specific.
- **udp.cxx** — recvmsg via FileReader, linked timeout for recv, sendto. DNS-internal.
- **file_io.cxx** — thin single-shot socket wrappers (socket/connect/send/recv/accept/shutdown). No multishot, no buffer rings, no timeouts, no batching. Socket methods grafted onto a file-oriented class.
- **client.cxx** — async send just landed using FileReader socket methods. Wrong tool.

Result: server has great perf but zero reuse. Client/DNS/UDP reinvent subsets. FileReader socket API is the worst of both — neither ergonomic nor fast.

## Goals

1. **Zero regression vs server** — multishot recv + buffer ring + direct fds + batch CQE must be first-class, not hidden behind abstractions that add overhead.
2. **Ergonomic Task\<T\> API** — simple coroutine interface for client-side and moderate-throughput use. `co_await stream.send(data)`, not "get SQE, prep, encode user_data, reserve completion slot."
3. **Raw batch API** — for server event loops that process thousands of CQEs/tick. No mandatory Task\<T\> overhead on the hot path.
4. **TCP + UDP + AF_ALG specializations** — not one generic socket class. Each protocol gets purpose-built types.
5. **Own the buffer ring** — buffer ring lifecycle, recycling, and recv integration in one place.
6. **Own generation counters** — stale CQE rejection built into the abstraction, not re-implemented per consumer.

## Non-goals

- Replace the server's event loop. Server keeps its dispatch loop; socket_io provides the building blocks it dispatches *to*.
- Async TLS. That's a separate subproject layered on top.
- Connection pooling. Higher-level concern.

---

## Core types

### SocketRing

Binds an `io_uring*` + `CompletionTable*` + encoder. Every socket op goes through this. Parallel to how FileReader binds a ring.

```cpp
class SocketRing {
    io_uring* ring_;
    CompletionTable* completions_;
    UserDataFn encode_ud_;
public:
    SocketRing(io_uring*, CompletionTable*, UserDataFn);

    io_uring* ring() noexcept;
    CompletionTable* completions() noexcept;
    u64 encode(u32 slot, u32 gen) noexcept;

    // SQE access for raw/batch API
    io_uring_sqe* get_sqe();
};
```

Lightweight, non-owning. Multiple can share a ring (e.g. server's file_io + socket_io on same ring).

### BufferRing

Owns a buffer ring group. Manages allocation, registration, recycling.

```cpp
struct BufferRingOptions {
    u32 count{4096};
    SZ buf_size{8192};
    u16 group_id{0};
    bool huge_pages{true};
};

class BufferRing {
public:
    BufferRing(io_uring*, BufferRingOptions);
    ~BufferRing(); // io_uring_free_buf_ring

    // Return a buffer to the kernel ring.
    void recycle(u16 buf_id) noexcept;

    // Batch recycle (server hot path).
    void recycle_batch(span<u16 const> ids) noexcept;

    // Access buffer data by id.
    span<byte> operator[](u16 buf_id) noexcept;

    u16 group_id() const noexcept;
    SZ buf_size() const noexcept;
    u32 count() const noexcept;
};
```

RAII-owned. One per buffer group. Server creates one, passes ref to socket_io ops.

### RecvBuffer

RAII handle to a single buffer from a BufferRing. Auto-recycles on destruction.

```cpp
class RecvBuffer {
    BufferRing* ring_;
    u16 buf_id_;
    SZ len_;
public:
    span<byte const> view() const noexcept;
    u16 id() const noexcept;
    SZ size() const noexcept;

    void release() noexcept; // early recycle
    ~RecvBuffer(); // recycle if not released
};
```

### DirectFdTable

Manages fixed file descriptor slot registration.

```cpp
class DirectFdTable {
public:
    DirectFdTable(io_uring*, u32 max_slots);
    ~DirectFdTable(); // io_uring_unregister_files

    // Register a listen fd into a specific slot.
    void install(u32 slot, int fd);
};
```

Server creates one. Accepted connections get direct slots automatically via multishot_accept_direct.

### GenTracker

Generation counter per fd/slot. Rejects stale CQEs.

```cpp
class GenTracker {
    V<u32> gens_;
public:
    GenTracker(u32 capacity);
    u32 current(int fd) const noexcept;
    u32 advance(int fd) noexcept; // increment + return new gen
    bool check(int fd, u32 gen) const noexcept; // gen == current?
};
```

---

## TCP API

### TcpStream

Connected TCP socket. Two API levels:

**Coroutine API** (client, moderate throughput):
```cpp
class TcpStream {
    SocketRing* ring_;
    FileHandle fh_;
public:
    // Connect to remote.
    static Task<TcpStream> connect(
        SocketRing&, sockaddr_storage addr, socklen_t len);

    // Construct from pre-connected fd (accepted socket).
    static TcpStream from_fd(SocketRing&, FileHandle fh);

    // Single-shot send. Returns bytes sent.
    Task<SZ> send(span<byte const> data, int flags = MSG_NOSIGNAL);

    // Send all data. Loops internally.
    Task<void> send_all(span<byte const> data, int flags = MSG_NOSIGNAL);

    // Single-shot recv into caller buffer. Returns bytes (0 = EOF).
    Task<SZ> recv(span<byte> buf);

    // Linked recv with timeout. Throws on timeout.
    Task<SZ> recv(span<byte> buf, chrono::milliseconds timeout);

    // Graceful close: shutdown(WR) → close.
    Task<void> close();

    // Access underlying handle for raw ops.
    FileHandle const& handle() const noexcept;
    SocketRing& ring() noexcept;
};
```

**Raw/batch API** (server hot path):

These are free functions that submit SQEs without Task overhead. The caller owns CQE dispatch.

```cpp
// Arm multishot recv with buffer ring selection.
// Returns {slot, gen} for CQE matching. Caller dispatches CQEs.
P<u32,u32> arm_recv_multishot(
    SocketRing&, int fd, BufferRing&,
    bool fixed_file = false, bool bundle = false);

// Re-arm after multishot ends (IORING_CQE_F_MORE absent).
P<u32,u32> rearm_recv_multishot(
    SocketRing&, int fd, BufferRing&,
    bool fixed_file = false, bool bundle = false);

// Submit single send. Returns {slot, gen}.
P<u32,u32> submit_send(
    SocketRing&, int fd, span<byte const> data,
    bool fixed_file = false);

// Submit writev (header + body). Returns {slot, gen}.
P<u32,u32> submit_writev(
    SocketRing&, int fd, span<iovec const> iov,
    bool fixed_file = false);

// Linked shutdown(WR) → close_direct. 2 SQEs.
void submit_shutdown_close(
    SocketRing&, int fd, u32 gen,
    bool fixed_file = false, bool direct = false);

// Async setsockopt via io_uring cmd_sock.
void submit_setsockopt(
    SocketRing&, int fd, int level, int optname,
    void const* optval, socklen_t optlen,
    bool fixed_file = false);
```

### TcpListener

```cpp
struct TcpListenOptions {
    bool reuse_addr{true};
    bool reuse_port{true};
    bool tcp_nodelay{true};
    bool tcp_quickack{true};
    bool ipv6_only{false};
    int backlog{SOMAXCONN};
};

class TcpListener {
public:
    // Bind + listen. Synchronous (cheap, done once).
    static TcpListener bind(
        SocketRing&, sockaddr_storage addr, socklen_t len,
        TcpListenOptions = {});

    // Single accept → Task<TcpStream>.
    Task<TcpStream> accept();

    // Arm multishot accept. Returns {slot, gen} for CQE dispatch.
    // direct=true → accepted fds go to direct fd table.
    P<u32,u32> arm_accept_multishot(bool direct = true);

    // Re-arm after IORING_CQE_F_MORE absent.
    P<u32,u32> rearm_accept_multishot(bool direct = true);

    FileHandle const& handle() const noexcept;
    int raw_fd() const noexcept;
};
```

---

## UDP API

```cpp
class UdpSocket {
public:
    static UdpSocket bind(SocketRing&, sockaddr_storage addr, socklen_t len);
    static UdpSocket ephemeral(SocketRing&, int family = AF_INET);

    Task<SZ> send_to(
        span<byte const> data, sockaddr_storage dest, socklen_t len,
        int flags = 0);

    Task<UdpRecvResult> recv_from(span<byte> buf, int flags = 0);

    // With linked timeout.
    Task<UdpRecvResult> recv_from(
        span<byte> buf, chrono::milliseconds timeout, int flags = 0);

    FileHandle const& handle() const noexcept;
};
```

Replaces the DNS-internal udp.cxx as the general UDP abstraction. DNS builds on this.

---

## Kernel Crypto — AF_ALG

AF_ALG gives the kernel's crypto implementations via socket API. io_uring can drive it async.

```cpp
struct CryptoAlgOptions {
    SV algorithm;    // e.g. "gcm(aes)", "sha256", "hmac(sha256)"
    span<byte const> key{};
    SZ tag_size{16}; // for AEAD
};

class CryptoAlg {
public:
    static Task<CryptoAlg> open(SocketRing&, CryptoAlgOptions);

    // AEAD encrypt: plaintext + iv + aad → ciphertext (includes tag).
    Task<V<byte>> encrypt(
        span<byte const> plaintext,
        span<byte const> iv,
        span<byte const> aad = {});

    // AEAD decrypt: ciphertext + iv + aad + tag → plaintext.
    // Returns unexpected on auth failure.
    Task<expected<V<byte>, CryptoError>> decrypt(
        span<byte const> ciphertext,
        span<byte const> iv,
        span<byte const> aad = {},
        span<byte const> tag = {});

    // Hash / HMAC: streaming.
    Task<void> update(span<byte const> data);
    Task<V<byte>> finalize();
};
```

This becomes a third backend alongside AESNI and pure-C++. Selection: AESNI > AF_ALG > fallback. Benchmark to validate — AF_ALG has syscall overhead that may lose to AESNI for small payloads.

**io_uring integration**: AF_ALG uses `sendmsg` (with cmsg for IV/op) and `read` on an accept'd socket. Both are io_uring-able. The SocketRing handles SQE submission; completions flow through the same CompletionTable.

---

## Server migration path

The server currently hand-rolls all io_uring socket ops. Migration is incremental — each piece can be swapped independently:

| Server component | socket_io replacement | Risk |
|---|---|---|
| Buffer ring setup + recycling | `BufferRing` | Low — isolated init/recycle |
| Direct fd table registration | `DirectFdTable` | Low — init only |
| Generation counter tracking | `GenTracker` | Low — drop-in |
| `queue_multishot_recv` | `arm_recv_multishot` | Low — same SQE pattern |
| `queue_multishot_accept` | `arm_accept_multishot` on TcpListener | Low |
| Send dispatch | `submit_send` / `submit_writev` | Low |
| Shutdown→close | `submit_shutdown_close` | Low |
| Async setsockopt | `submit_setsockopt` | Low |
| CQE dispatch loop | **Stays in server** | N/A — socket_io doesn't own the loop |

The server keeps its event loop and Op-tagged dispatch. socket_io provides the building blocks. No perf regression because the raw API submits the exact same SQEs the server currently does — just from reusable functions instead of inline code.

## Client migration

Current async client (using FileReader) migrates to:

```cpp
Task<HttpResult> do_async_plaintext_request(
    SocketRing& sring, HttpRequest const& req, HttpClientOptions const& opts) {
    // ...
    auto stream = co_await TcpStream::connect(sring, addr, addr_len);
    co_await stream.send_all(as_bytes(span{wire}));
    // recv headers
    while (...) {
        auto n = co_await stream.recv(buf);
        // ...
    }
    co_await stream.close();
    co_return response;
}
```

FileReader socket methods become dead code once socket_io is adopted.

## DNS migration

dns.cxx currently creates per-query temp io_uring rings for resolve_blocking. With socket_io:
- `UdpSocket` replaces hand-rolled socket creation
- Linked timeouts are built into `recv_from(buf, timeout)`
- `resolve_blocking` can reuse a thread-local SocketRing instead of creating temp rings
- Async `resolve()` uses caller's SocketRing directly — no cross-ring problem

---

## What the raw API buys the server (perf parity)

The server processes ~100K CQEs/sec per ring. Every layer matters:

1. **No Task\<T\> on recv hot path** — `arm_recv_multishot` returns a `{slot, gen}` pair. Server's CQE loop calls `dispatch(slot, gen, res, flags)` directly. Zero coroutine frame allocation.

2. **Buffer ring stays kernel-managed** — `RecvBuffer` is sugar for non-hot paths. Server can call `buf_ring[buf_id]` + `buf_ring.recycle(buf_id)` directly without RAII overhead.

3. **Batch recycle** — `recycle_batch(ids)` advances the ring once for N buffers instead of N times.

4. **Same SQE flags** — `arm_recv_multishot` sets `IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE | ioprio BUNDLE` exactly like the server does today. No flag abstraction.

5. **GenTracker is array + compare** — no hash map, no allocation on the hot path.

The ergonomic `TcpStream` API is for code that isn't in the CQE-per-microsecond path. The raw free functions are for code that is.

---

## File layout

```
src/socket_io/
    socket_io.cxx    — module conflux.socket_io
                        BufferRing, DirectFdTable, GenTracker, SocketRing
                        TcpStream, TcpListener (coroutine API)
                        UdpSocket
                        Raw TCP/UDP free functions (batch API)
                        RecvBuffer

src/socket_io/
    crypto_alg.cxx   — module conflux.socket_io:crypto_alg (partition)
                        CryptoAlg (AF_ALG wrapper)
                        Behind #if CONFLUX_HAS_KERNEL_CRYPTO
```

Single TU for the main module. Crypto as a partition because it's optional and has its own kernel feature detection.

---

## Open questions

1. **send_zc** — server doesn't use it today but FileReader has it. Include in raw API? Benchmark first.
2. **Multishot recv stream** — ergonomic coroutine wrapper for multishot recv. `AsyncGenerator<RecvBuffer>`? Or leave raw-only for now?
3. **io_uring_prep_connect timeout** — no kernel-native connect timeout. Linked timeout works but adds SQE. Worth the ergonomics?

## Decisions (from review)

- **AF_ALG deferred** — not in first pass. Separate partition later, benchmark-gated.
- **Raw APIs take explicit `u64 user_data`** — no CompletionTable involvement. Server keeps its Op packing.
- **SocketRawRing vs SocketTaskRing** — separate types. Raw ring has no CompletionTable. Task ring (coroutine API, future pass) wraps raw ring + CompletionTable + UserDataFn.
- **`_borrowed` suffix** — raw send/writev/connect signal that caller owns buffer lifetime until CQE.
- **RecvBuffer** — `release()` = recycle now, `detach()` = skip recycle in destructor. Server raw path uses `buffer_view()` + `recycle()` directly, no RAII.
- **recycle_batch()** — N adds then one advance, not N separate advances.
- **Implementation order** — raw server primitives first, coroutine wrappers later.

## Benchmarking gate

Every server migration step MUST pass `--compare-bins` non-regression on release builds before merging. Methodology:

1. Build release baseline (pre-migration): `cmake --preset release-clang-libcxx && cmake --build --preset release-clang-libcxx -j1`
2. Stash baseline binaries
3. Apply migration
4. Build release of migrated code
5. `bench_record.sh --compare-bins baseline:/path/old migrated:/path/new`
6. Check `bench_compare_summary` — any regression → reevaluate API boundaries (parameter count, struct vs direct args, inlining, etc.)

Existing benchmarks: `conflux_benchmarks` (routing), `conflux_tcp_increment_coro_bench` (io_uring socket).

**Gap**: No HTTP server transport benchmark exists. Before step 2, add a server throughput microbench (multishot accept → recv → parse → send → close cycle) so migration regressions in the CQE hot path are visible.

## Implementation order

- [x] 1. Raw submission layer + BufferRing + DirectFdTable + GenerationTable
- [x] 1b. Server transport benchmarks (conflux_http_server_bench, conflux_http_server_concurrency_bench, conflux_socket_raw_bench)
- [x] 2. Migrate server buffer ring setup/recycling to BufferRing
- [x] 3. Migrate server direct fd setup to DirectFdTable
- [x] 4. Migrate server multishot accept/recv to raw submit functions
- [x] 5. Migrate server send/writev/shutdown-close/setsockopt
- [x] 6. Coroutine wrappers: TcpStream, UdpSocket (conflux.socket_io.coro module; split to fix GCC 16 diamond import bug)
- [ ] 7. Async HTTP client using TcpStream (plaintext; TLS stays blocking)
- [x] 8. Migrate DNS UDP transport to UdpSocket
- [ ] 9. Deprecate/remove FileReader socket methods once step 7 complete
- [ ] 10. AF_ALG — benchmark-gated, deferred
