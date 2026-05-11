# P2-G: Registered Send-Buffer Pool for HTTP Responses

Date: 2026-05-10
Status: EXPERIMENTAL — implement on branch only, benchmark before merge
Effort: 2–3 days
Prerequisite: none (independent of SEND_ZC)
Expected payoff: marginal for copy-based path, plausible for direct-format short responses

## Problem

HTTP response headers are assembled into `std::string Conn::own_response`
(heap-allocated) and sent via `submit_send_borrowed()` → `io_uring_prep_send`.
Every send forces the kernel to page-map the heap buffer. For high-RPS
short-response workloads, per-send page mapping overhead is measurable.

`FixedBufferPool` already exists in `file_io.cxx` for read-side registered
buffers. The write/send side has no pool — registered buffers are unused for
network sends.

## Current Send Path

```
format_response() → S own_response      (heap std::string)
queue_send(fd)    → submit_send_borrowed(ring, handle, own_response.data(), ...)
                  → io_uring_prep_send(sqe, fd, data, len, flags)
handle_send()     → partial: resubmit with offset
                  → complete: clear own_response, rearm recv
```

Three send paths: plain (SEND), mapped file (WRITEV with iov[0]=header
iov[1]=mmap), streamed file (SEND header + splice body). All use unregistered
heap buffers for headers.

## Existing Infrastructure

| Component | File | Status |
|---|---|---|
| `FixedBufferPool` (alloc/free/register) | `file_io.cxx:74-139` | Done (read-side only) |
| `FixedBuffer` RAII wrapper | `file_io.cxx:49-73` | Done |
| `sqe.prep_send_zc_fixed()` | `uring.cxx:488-504` | Done (unused by server) |
| `io_uring_register_buffers_sparse` | `file_io.cxx:101` | Done (read pool) |
| `IoUringCaps::send_zc` detection | `uring.cxx:1587-1589` | Done |
| `ioprio_flags::recvsend_fixed_buf` | `uring.cxx` | Done |

## Design

### Step 1: Shared Registered Buffer Table

Current `FixedBufferPool` owns the full registration lifecycle: ctor calls
`io_uring_register_buffers_sparse()`, dtor calls `io_uring_unregister_buffers()`.
A second pool on the same ring would fail registration or unregister the
other pool's buffers. Fix this first.

Split into two types:

```cpp
struct RegisteredBufferTable {
    io_uring* ring{};
    bool registered{};

    explicit RegisteredBufferTable(io_uring& r, u32 total_slots);
    // total_slots must be <= UINT16_MAX (SQE buf_index is u16)
    ~RegisteredBufferTable(); // only owner that calls io_uring_unregister_buffers

    bool update(u32 idx, iovec const& iov);
};

struct FixedBufferPool {
    RegisteredBufferTable* table{};
    u32 base{};
    u32 count{};
    SZ slab_bytes{};
    V<unsigned> free{};          // local indices [0, count)
    V<UPD<byte[], void(*)(void*)>> slabs{};

    Opt<FixedBuffer> try_acquire();
};

struct FixedBuffer {
    u32 local_slot;              // indexes slabs_
    FixedBufferPool* pool;

    u32 slot() const noexcept { return pool->base + local_slot; } // absolute, for io_uring buf_index
    span<byte> view() noexcept  { return pool->view_for(local_slot); }
    byte* data() noexcept       { return view().data(); }
    byte const* data() const noexcept;
};
```

Slot layout:
```
file pool: slots [0, file_io_slabs)
send pool: slots [file_io_slabs, file_io_slabs + send_buffer_slabs)
```

One `RegisteredBufferTable` per ring. Pools only own index slices.

### Step 2: SQE `buf_index` Helper

```cpp
// uring.cxx
inline Sqe& buf_index(FixedBufIdx idx) noexcept {
    assert(idx.v <= std::numeric_limits<u16>::max());
    raw()->buf_index = static_cast<u16>(idx.v);
    return *this;
}
```

### Step 3: Fixed Send Submit Helper

```cpp
// socket_io.cxx
export bool submit_send_fixed_borrowed(
    SocketRawRing& ring,
    SocketHandle handle,
    u32 buf_idx,
    void const* data,
    SZ len,
    u64 user_data,
    int msg_flags = MSG_NOSIGNAL
) {
    auto sqe = ring.try_get_sqe();
    if (!sqe)
        return false;

    sqe.prep_send(
        handle.sqe_fd(),
        data,
        len,
        conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)}
    );
    sqe.add_flags(handle.sqe_fd_flags()); // direct fd — NOT fixed buffer
    sqe.ioprio(conflux::uring::ioprio_flags::recvsend_fixed_buf);
    sqe.buf_index(conflux::uring::FixedBufIdx{buf_idx});
    sqe.user_data(conflux::uring::UserData{user_data});
    return true;
}
```

**Critical:** `IOSQE_FIXED_FILE` is for registered file descriptors, not
registered buffers. Fixed-buffer `IORING_OP_SEND` requires
`IORING_RECVSEND_FIXED_BUF` in `sqe->ioprio` with `buf_index` set.
`IOSQE_FIXED_FILE` only comes from `handle.sqe_fd_flags()`.

References: [io_uring_enter2(2)](https://man7.org/linux/man-pages/man2/io_uring_enter.2.html),
[liburing io_uring.h](https://github.com/axboe/liburing/blob/master/src/include/liburing/io_uring.h)

### Step 4: Conditional Registered Send (copy-based)

Additional `Conn` state for fixed-buffer offset tracking:

```cpp
struct Conn {
    // ... existing fields ...
    Opt<FixedBuffer> send_buf;
    SZ send_buf_base_written{};  // conn.written at copy time — slab[0] = response[base_written]
    SZ send_buf_len{};           // bytes copied into slab
};
```

The slab holds `response[send_buf_base_written .. send_buf_base_written + send_buf_len)`.
On partial send, slab-local offset is `conn.written - conn.send_buf_base_written`.

In `queue_send()`:

```cpp
void queue_send(int fd) {
    auto& conn = fd_table[ufd];
    auto gen = conn.gen;
    SZ len = conn.own_response.size() - conn.written;

    // Partial send with existing fixed buffer — resubmit from same slab
    if (conn.send_buf.valid()) {
        assert(conn.written >= conn.send_buf_base_written);
        auto local_off = conn.written - conn.send_buf_base_written;
        assert(local_off <= conn.send_buf_len);
        auto remaining = conn.send_buf.view().subspan(local_off,
                                                       conn.send_buf_len - local_off);
        if (!submit_send_fixed_borrowed(raw_, handle, conn.send_buf.slot(),
                                        remaining.data(), remaining.size(),
                                        pack(Op::Send, gen, fd))) {
            defer_op([this, fd, gen] {
                auto ufd = to_ufd(fd);
                if (fd_table[ufd].gen == gen) queue_send(fd);
            });
        }
        return;
    }

    // Try acquire a new fixed buffer for first send
    if (send_buffers_ && ring.send_fixed_buffers_supported && len <= send_buffers_->slab_bytes()) {
        auto buf = send_buffers_->try_acquire();
        if (buf) {
            auto view = buf->view().subspan(0, len);
            std::memcpy(view.data(), conn.own_response.data() + conn.written, len);

            if (submit_send_fixed_borrowed(raw_, handle, buf->slot(), view.data(), view.size(),
                                           pack(Op::Send, gen, fd))) {
                conn.send_buf = std::move(*buf);
                conn.send_buf_base_written = conn.written;
                conn.send_buf_len = len;
                return;
            }

            // No SQE available — release buffer, defer retry
            defer_op([this, fd, gen] {
                auto ufd = to_ufd(fd);
                if (fd_table[ufd].gen == gen) queue_send(fd);
            });
            return;
        }
    }
    // Fallback: unregistered send (current path)
    if (!submit_send_borrowed(raw_, handle, conn.own_response.data() + conn.written, len,
                              pack(Op::Send, gen, fd))) {
        defer_op([this, fd, gen] {
            auto ufd = to_ufd(fd);
            if (fd_table[ufd].gen == gen) queue_send(fd);
        });
    }
}
```

In `handle_send()`, old-kernel `-EINVAL` fallback (acceptance criterion 8):

```cpp
if (res == -EINVAL && conn.send_buf.valid()) {
    // Kernel does not support IORING_RECVSEND_FIXED_BUF — disable and retry borrowed
    ring.send_fixed_buffers_supported = false;
    conn.send_buf.reset();
    conn.send_buf_base_written = 0;
    conn.send_buf_len = 0;
    queue_send(fd); // takes borrowed fallback path from now on
    return;
}
```

The SQE submits successfully but completes with `-EINVAL` on kernels that
don't support the modifier. At that point the fixed buffer is still held and
`conn.written` has not advanced. After disabling, all future sends on this
ring use the borrowed path.

On full send completion or close/error in `handle_send()`:

```cpp
conn.send_buf.reset();
conn.send_buf_base_written = 0;
conn.send_buf_len = 0;
```

Key invariants:
- Slab offset tracking: `send_buf_base_written` records `conn.written` at copy
  time. Partial resubmit computes local offset as `conn.written - send_buf_base_written`.
  This is correct even if `conn.written > 0` when the slab was first filled.
- If `submit_send_fixed_borrowed()` fails (no SQE), no buffer is retained —
  the send is deferred safely.
- Partial sends resubmit remaining slice from the same registered buffer — do
  not reacquire/copy.
- `conn.send_buf.reset()` only on full completion or close/error (not on
  partial-send CQE).
- Deferred retries capture `gen` and check it before acting — a closed/reused
  fd does not revive a stale send.

### Step 5 (conditional): Direct Format into Slab

Copy-based path adds `own_response → slab → kernel`. For tiny `/ping`
responses, that extra `memcpy` may eat most of the fixed-buffer win.

If step 4 benchmark is neutral or positive, add direct formatting:

```
acquire send slab
format HTTP response directly into slab
submit SEND with IORING_RECVSEND_FIXED_BUF
hold slab until send completion
```

Removes `std::string` allocation/capacity churn and the extra copy. If direct
formatting wins, keep P2-G. If copy-based is neutral/negative and direct
formatting too invasive, drop P2-G → spend time on T1-A `SEND_ZC_FIXED`.

## Config

```ini
[server]
send_buffer_slabs = 64
send_buffer_bytes = 4096       # 4 KiB — most headers fit; pinned against RLIMIT_MEMLOCK
send_fixed_buffers = false     # enable only after benchmark confirms win
```

Registered buffers are pinned against `RLIMIT_MEMLOCK`; huge pages can pin
whole pages even if only partially used.

## Interaction with SEND_ZC

Keep separate from T1-A but design compatible. `prep_send_zc_fixed()` already
exists and requires registered buffers with `buf/len` inside the indexed buffer.

Suggested thresholds:
```
<= 4 KiB plain small response:  SEND + IORING_RECVSEND_FIXED_BUF, direct format into slab
4 KiB..64 KiB:                  benchmark both SEND fixed and SEND_ZC_FIXED
large static body:              SEND_ZC / splice path, not P2-G
```

Do not use `WRITE_FIXED` for socket response path — it is `write(2)`-style,
not `send(2)`-style, no `MSG_NOSIGNAL`.

## Not in Scope

- SEND_ZC integration (separate proposal, T1-A).
- Dynamic pool resizing.
- Registered buffers for mapped-file body sends.
- TLS path — TLS already writes encrypted bytes into `tls_send_buf`; fixed
  buffers only help after a different TLS send-buffer strategy.

## Acceptance Criteria

1. Buffer registration is single-owner; no second `io_uring_register_buffers_sparse()` on same ring.
2. File fixed-buffer path still works after refactor.
3. Plain HTTP small responses use `IORING_RECVSEND_FIXED_BUF`, verified by code path and perf counter/log.
4. Partial sends resubmit from the same registered buffer.
5. Pool exhaustion falls back to current `submit_send_borrowed()`.
6. Close/error paths release the buffer.
7. Direct fd mode and fixed-buffer mode not confused: `IOSQE_FIXED_FILE` only from `SocketHandle::sqe_fd_flags()`.
8. Old-kernel unsupported modifier → clean fallback or disable feature after first `-EINVAL`.
9. Benchmarks include `/ping` with keepalive + high concurrency, not only `tcp_increment_coro_bench`.
10. If `submit_send_fixed_borrowed()` cannot obtain an SQE, no buffer is retained and the send is deferred safely.

## Benchmark Gate

No T1/T2 item lands without benchmark + perf/fdinfo data on same hardware
under realistic HTTP load.

Required benchmarks:
- `tcp_increment_coro_bench` (--compare-bins, release) — no regression
- Short-response HTTP bench (`wrk -c 100 -t 4` against `/ping`) — measure RPS delta
- `perf stat` for page-fault / TLB-miss reduction
- Ring fdinfo counters

Hypothesis: 5–15% RPS gain for small responses (copy-based path may be lower end or neutral).
