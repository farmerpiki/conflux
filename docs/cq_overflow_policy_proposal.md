# CQ Overflow Policy Proposal (P0-05)

Status: accepted, pre-implementation (amended post-review ×4)
Date: 2026-05-09

## Problem

A lost or unprocessed CQE in a managed flow can leak a buffer slot or direct
slot permanently. The direct-file-flow design treats this as an integration
responsibility, but no enforcement or detection exists at the ring or server
level today.

`RingRef::cq_has_overflow()` now wraps `io_uring_cq_has_overflow()`.
`Ring::cq_has_overflow()` wraps the same for owning rings.
That is the only piece currently done.

## What "CQ overflow" means

### With `IORING_FEAT_NODROP` (modern kernels)

The kernel stores overflowed CQEs in an internal overflow list rather than
dropping them. `io_uring_cq_has_overflow()` returns true when that list is
non-empty — meaning CQEs are waiting to be flushed into the CQ ring, not
necessarily lost. `io_uring_get_events()` flushes outstanding overflow CQEs
into the CQ ring and is the correct mechanism to drain them before teardown.

Overflow can also cause `io_uring_enter()`/`io_uring_submit_and_wait()` to
return `-EBADR` when the kernel would otherwise block waiting for the CQ to
drain.

### Without `IORING_FEAT_NODROP`

CQEs may be silently dropped. Overflow is immediate integrity loss.

### Policy framing

> CQ overflow means the ring lifecycle state is no longer within the server's
> intended operating envelope. With `IORING_FEAT_NODROP`, attempt bounded
> overflow flushing before fatal shutdown. Without `IORING_FEAT_NODROP`, treat
> overflow as immediate integrity loss and shut down immediately.

Managed flows cannot tolerate lost CQEs. A lost close-direct CQE means the
slot stays in `closing` and the file descriptor leaks. A lost recv CQE means
the buffer slot is never recycled.

## Policy Decision

**A. Fatal ring error (chosen):** On any overflow event, set `ring_fatal_`,
flush with `io_uring_get_events()`, drain remaining CQEs as best-effort
including recv buffer recycling, close tracked fds synchronously, then return
from the run loop. `run()` signals the fatal state to the caller. Clean restart
required. Simple to reason about; correct for correctness-critical deployments.

**B. Ring quarantine (deferred to P2):** Stop submitting new SQEs to the
overflowed ring, drain all pending CQEs, then resume. Allows recovery without
restart. More complex; adds a second operational mode. Defer until overflow
telemetry proves it worth the complexity.

### Fatal signalling — no throw, non-optional return

Exceptions (`throw`) are reserved for truly exceptional startup/config
failures. The fatal overflow path is a runtime shutdown: use a stored error
code and a non-optional return status.

`run()` must not return as if normal after a fatal shutdown. The caller should
not have to query a separate `fatal_reason()` accessor to discover the outcome:

```cpp
enum class RunStatus : u8 {
    stopped_normally,
    fatal_cq_overflow,
    fatal_cq_overflow_no_nodrop,
    fatal_submit_wait_ebadr,
    fatal_internal_exception,  // unexpected exception caught at run() boundary
};

[[nodiscard]] RunStatus HttpServer::run() noexcept;
```

`run() noexcept` requires that every internal exception is caught and mapped
to a `RunStatus`. Startup/config exceptions are no exception: catch at the
`run()` boundary and return `fatal_internal_exception`. This makes the noexcept
contract self-consistent and avoids `std::terminate` on uncaught throws.

`fatal_cq_overflow_flush_limit` is removed from `RunStatus` — flush-limit is a
diagnostic modifier on the primary cause, not a distinct trigger (see Fatal
Reason Fields below).

Hot-path cost: identical — a single bool load at loop top.
Cold-path (shutdown): no unwinding overhead.

### Fatal reason — typed enum, not optional string

```cpp
enum class ServerFatalReason : u8 {
    none,
    cq_overflow,
    cq_overflow_no_nodrop,
    submit_wait_ebadr,
    internal_exception,
};
```

### Fatal Reason Fields

Root cause and flush outcome are stored separately to avoid overwriting the
trigger with cleanup status:

```cpp
ServerFatalReason fatal_reason_{ServerFatalReason::none};
bool overflow_flush_limit_hit_{false};
u32 fatal_cq_overflow_count_{0};
bool ring_fatal_{false};
```

`overflow_flush_limit_hit_` is set at the end of
`flush_overflow_cqes_until_clear_or_limit()` if overflow is still non-zero
after the iteration cap. It is a diagnostic annotation on the primary reason,
not a replacement for it.

No allocation, stable diagnostics, easy switch-based tests. Maps cleanly to
`RunStatus` on return.

## Non-Goals

- Do not change CQ size selection at startup (future config item).
- Do not add ring resize logic here (P2-01).
- Do not add per-flow CQE accounting; use ring-level overflow flag.

## Changes Required

### Prerequisite: fix `io_uring_submit_and_wait` error handling in `http_server`

**Must be done before adding overflow policy.**

liburing helpers return `-errno`, not `-1` with `errno` set. Current code
checks `errno` after a negative return — wrong. Correct shape with explicit
NODROP branching:

```cpp
int const rc = io_uring_submit_and_wait(&ring, 1);
if (rc < 0) {
    if (rc == -EINTR)
        continue;
    if (rc == -EBADR || raw_.ring().cq_has_overflow()) {
        if (!raw_.ring().has_feature(IORING_FEAT_NODROP)) {
            enter_ring_fatal(ServerFatalReason::cq_overflow_no_nodrop);
            close_tracked_fds_sync();
            return RunStatus::fatal_cq_overflow_no_nodrop;
        }
        enter_ring_fatal(ServerFatalReason::submit_wait_ebadr);
        flush_overflow_cqes_until_clear_or_limit();
        return RunStatus::fatal_submit_wait_ebadr;
    }
    continue;
}
```

### `src/uring/uring.cxx`

Already done: `RingRef::cq_has_overflow()`, `Ring::cq_has_overflow()`.

Add `cq_overflow_count()` to **both** `RingRef` and `Ring`. `koverflow` is a
mapped pointer — must be dereferenced:

```cpp
[[nodiscard]] u32 RingRef::cq_overflow_count() const noexcept {
    assert(ring_ != nullptr);
    auto* p = ring_->cq.koverflow;
    return p != nullptr ? *p : 0u;
}

[[nodiscard]] u32 Ring::cq_overflow_count() const noexcept {
    auto* p = ring_.cq.koverflow;
    return p != nullptr ? *p : 0u;
}
```

Add `RingRef::has_feature()` for clean NODROP branching:

```cpp
[[nodiscard]] bool RingRef::has_feature(u32 feature) const noexcept {
    assert(ring_ != nullptr);
    return (ring_->features & feature) != 0u;
}
```

### `src/net/http_server.cxx`

#### Fatal mode fields

```cpp
ServerFatalReason fatal_reason_{ServerFatalReason::none};
u32 fatal_cq_overflow_count_{0};
bool ring_fatal_{false};
```

Block all new submissions when fatal:

```cpp
io_uring_sqe* get_sqe() noexcept {
    if (ring_fatal_) return nullptr;
    auto sqe = raw_.try_get_sqe();
    return sqe ? sqe.raw() : nullptr;
}

void defer_op(detail::small_move_only_function<void()> op) {
    if (ring_fatal_) return;
    pending_ops_.push_back(std::move(op));
}
```

Enter fatal state:

```cpp
void enter_ring_fatal(ServerFatalReason reason) noexcept {
    ring_fatal_ = true;
    shutting_down_ = true;
    pending_ops_.clear();
    fatal_reason_ = reason;
    fatal_cq_overflow_count_ = raw_.ring().cq_overflow_count();
}
```

#### Overflow check — before `drain_pending_ops()`

Overflow must be checked **before** submitting deferred ops. Submitting into
an overflowed ring worsens the situation. Branch on NODROP explicitly:

```cpp
for (;;) {
    if (ring_integrity_suspect()) {
        if (!raw_.ring().has_feature(IORING_FEAT_NODROP)) {
            enter_ring_fatal(ServerFatalReason::cq_overflow_no_nodrop);
            close_tracked_fds_sync();
            return RunStatus::fatal_cq_overflow_no_nodrop;
        }
        enter_ring_fatal(ServerFatalReason::cq_overflow);
        flush_overflow_cqes_until_clear_or_limit();
        return RunStatus::fatal_cq_overflow;
    }

    drain_pending_ops();

    int const rc = io_uring_submit_and_wait(&ring, 1);
    if (rc < 0) {
        if (rc == -EINTR)
            continue;
        if (rc == -EBADR || ring_integrity_suspect()) {
            enter_ring_fatal(ServerFatalReason::submit_wait_ebadr);
            flush_overflow_cqes_until_clear_or_limit();
            return RunStatus::fatal_submit_wait_ebadr;
        }
        continue;
    }

    A<io_uring_cqe*, BATCH> cqes{};
    unsigned const count = io_uring_peek_batch_cqe(&ring, cqes.data(), BATCH);

    bool const overflowed = ring_integrity_suspect();

    if (count == 0) {
        if (overflowed) {
            enter_ring_fatal(ServerFatalReason::cq_overflow);
            flush_overflow_cqes_until_clear_or_limit();
            return RunStatus::fatal_cq_overflow;
        }
        continue;
    }

    // ... normal CQE dispatch ...
}
```

#### `ring_integrity_suspect()`

```cpp
[[nodiscard]] bool ring_integrity_suspect() const noexcept {
    return raw_.ring().cq_has_overflow();
}
```

#### Bounded overflow flush — must recycle recv buffers

`flush_overflow_cqes_until_clear_or_limit()` cannot blindly dispatch CQEs
through the normal dispatcher. The normal dispatcher can queue more
accept/recv/send/close work via `get_sqe()`. The `ring_fatal_` guard on
`get_sqe()` prevents new submissions, but the recv-buffer recycling path is
two-phase in the normal loop. The drain must complete both phases to avoid
leaking recv buffer IDs:

```cpp
void flush_overflow_cqes_until_clear_or_limit() noexcept {
    // ring_fatal_ must already be true before entering
    constexpr unsigned max_iters = 16;
    for (unsigned i = 0; i < max_iters && ring_integrity_suspect(); ++i) {
        io_uring_get_events(&ring);  // flush overflow list → CQ ring
        A<io_uring_cqe*, BATCH> cqes{};
        unsigned const n = io_uring_peek_batch_cqe(&ring, cqes.data(), BATCH);
        if (n == 0) break;
        for (unsigned j = 0; j < n; ++j)
            dispatch_cqe_fatal(cqes[j]);
        io_uring_cq_advance(&ring, n);
    }
    if (ring_integrity_suspect())
        overflow_flush_limit_hit_ = true;   // annotates primary reason; does not replace it
    // diagnostics emitted here before close
    close_tracked_fds_sync();
}
```

#### `dispatch_cqe_fatal()` — explicit op switch

`dispatch_cqe_fatal()` is a trimmed dispatcher. Behavior must be defined per
op — not left as a vague "minimal dispatcher":

```cpp
void dispatch_cqe_fatal(io_uring_cqe const* cqe) noexcept {
    auto const [op, fd] = decode_user_data(cqe->user_data);
    switch (op) {
    case Op::Recv:
        recycle_recv_buffer_direct(cqe);   // recycles directly, no deferred phase
        break;
    case Op::Accept:
        close_or_track_accepted_fd(cqe);   // best-effort; sync closed below
        break;
    case Op::DirectSlotClose:
    case Op::Send:
    case Op::Timeout:
    case Op::FileRead:
        account_only(cqe);
        break;
    default:
        log_unknown_fatal_cqe(op, cqe->user_data);
        break;
    }
}
```

This avoids "minimal dispatcher" becoming a vague second dispatcher with
hidden invariants.

#### Emergency fd cleanup

Normal shutdown closes fds via CQEs. Fatal exit skips that. Close
synchronously before returning from the fatal path:

```cpp
void close_tracked_fds_sync() noexcept {
    for (auto& conn : fd_table_) {
        if (conn.fd >= 0) {
            ::close(conn.fd);
            conn.fd = -1;
        }
    }
}
```

For direct/fixed-file mode: confirm `DirectFdTable::unregister_files()`
releases accepted direct slots. If not guaranteed, that is a separate gap.

### Drain on shutdown

`http_server` does not own a `FlowRuntime`. The shutdown path needs a ring
drain, not a flow drain. Do not add `drain_deferred_closes()` to the HTTP
server unless the server acquires a `FlowRuntime`. If a flow-aware drain is
needed later, the correct split is:

```cpp
flow_runtime.retry_deferred_close_submissions();
flow_runtime.drain_deferred_close_completions(deadline);
```

### Diagnostics

Export on shutdown, including `/proc/self/fdinfo` counters for the ring fd:

```
ring_features=NODROP,FAST_POLL,SUBMIT_STABLE,...
ring_sq_dropped=N
ring_sq_busy=N
ring_cq_overflow=N
ring_cq_overflow_delta=N          // change since last diagnostic emission
ring_fatal_reason=cq_overflow     // only when fatal path triggered
ring_overflow_flush_limit_hit=1   // only when flush loop exhausted without clearing overflow
```

`ring_fatal_reason` records the trigger. `ring_overflow_flush_limit_hit`
records the cleanup outcome. Both can appear together.

`fdinfo` parsing is Linux-only; this project already is Linux/io_uring-specific.

### Startup capability log (P1)

At ring init, emit a single structured capability line:

```
uring_features=NODROP,FAST_POLL,SUBMIT_STABLE,...
uring_setup_flags=SINGLE_ISSUER,DEFER_TASKRUN,COOP_TASKRUN,...
```

This makes overflow reports and NODROP branching observable without log archaeology.

### Stress test

Add a deterministic ring-level test that induces overflow via a tiny CQ
(bypass the HTTP server entirely). Add a separate HTTP stress test with a
small CQ that verifies non-UB shutdown; mark it flaky-guarded since CQ size 4
does not guarantee overflow deterministically across kernels/schedulers.

## Verification Targets

- `cq_has_overflow()` true when overflow list non-empty.
- `cq_overflow_count()` dereferences `koverflow` pointer; monotonically increases.
- `has_feature(IORING_FEAT_NODROP)` available on `RingRef`; returns false on old kernels.
- Without NODROP: `enter_ring_fatal` + `close_tracked_fds_sync`, no flush attempted.
- With NODROP: `flush_overflow_cqes_until_clear_or_limit` runs, recv bufs recycled.
- Overflow check fires **before** `drain_pending_ops()` in the run loop.
- Overflow check also fires before `count == 0` early continue.
- `ring_fatal_` blocks `get_sqe()` and `defer_op()` immediately.
- `dispatch_cqe_fatal()` handles every `Op` explicitly; unknown ops logged.
- `close_tracked_fds_sync()` called before returning from fatal path.
- `io_uring_submit_and_wait` negative returns handled as `-errno`, not `errno`.
- `-EBADR` triggers fatal path.
- `run()` returns `RunStatus` — never `stopped_normally` after fatal event.
- Unexpected exception in `run()` caught at boundary, returns `fatal_internal_exception`.
- `overflow_flush_limit_hit_` set only when flush exhausted; does not overwrite `fatal_reason_`.
- Diagnostics emit `ring_cq_overflow=N`, `ring_fatal_reason=...`, fdinfo counters.
- Ring-level deterministic test + best-effort HTTP stress test pass without UB.

## Implementation Order

1. **Fix `io_uring_submit_and_wait` error handling** in `http_server`. ← prerequisite
2. Add `cq_overflow_count()` to `RingRef` and `Ring`; add `RingRef::has_feature()`.
3. Add `ServerFatalReason` enum; add `fatal_reason_` / `fatal_cq_overflow_count_` / `ring_fatal_`.
4. Change `run()` signature to `[[nodiscard]] RunStatus run() noexcept`.
5. Add `enter_ring_fatal(ServerFatalReason)` + `close_tracked_fds_sync()`.
6. Add loop-top overflow check with explicit NODROP branch before `drain_pending_ops()`.
7. Add post-peek overflow check before `count == 0` continue.
8. Add `dispatch_cqe_fatal()` with explicit op switch.
9. Add `flush_overflow_cqes_until_clear_or_limit()` with recv-buffer recycling.
10. Wire `RunStatus` return from all fatal return sites.
11. Export diagnostics on shutdown including fdinfo counters.
12. Add deterministic ring-level test; add best-effort HTTP stress test (flaky-guarded).
13. **(P1)** Add startup capability log (`uring_features=...`).

## Classification

| Item                                                  | Priority | Decision                                                         |
| ----------------------------------------------------- | -------: | ---------------------------------------------------------------- |
| Fix submit_and_wait errno handling                    |       P0 | Prerequisite; implement first                                    |
| `ServerFatalReason` enum (replace `optional<string>`) |       P0 | No allocation; stable diagnostics; switch-friendly               |
| `[[nodiscard]] RunStatus run() noexcept`              |       P0 | Non-optional fatal signal; caller cannot ignore                  |
| Fatal-on-overflow policy with NODROP branch           |       P0 | Implement with explicit has_feature() branching                  |
| `cq_overflow_count()` on `RingRef` and `Ring`         |       P0 | Implement; dereference `koverflow` pointer                       |
| `RingRef::has_feature()`                              |       P0 | Add; needed for clean NODROP branching                           |
| `ring_fatal_` flag + `get_sqe()`/`defer_op()` guard  |       P0 | Implement; prevents submitting into overflowed ring              |
| Overflow check before `drain_pending_ops()`           |       P0 | Implement; not just after peek                                   |
| Overflow check before `count == 0` continue           |       P0 | Implement                                                        |
| `dispatch_cqe_fatal()` with explicit op switch        |       P0 | Implement; not a vague "minimal dispatcher"                      |
| Bounded overflow flush with recv-buf recycling        |       P0 | Implement; separate from normal two-phase path                   |
| Emergency `close_tracked_fds_sync()`                  |       P0 | Implement before returning from fatal path                       |
| Diagnostics with fdinfo counters                      |       P0 | `ring_sq_dropped`, `ring_sq_busy`, `ring_cq_overflow`, delta     |
| Startup capability log                                |       P1 | `uring_features=...` at init                                     |
| Generic `drain_deferred_closes()` in HTTP server      |    P1/P2 | Defer unless server owns `FlowRuntime`                           |
| Ring quarantine / resume                              |       P2 | Defer; more state, worse ergonomics                              |
| CQ sizing config / resize                             |       P2 | Separate proposal                                                |
