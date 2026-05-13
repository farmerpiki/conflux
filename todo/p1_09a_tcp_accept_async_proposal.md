# P1-09a Proposal: Async TcpListener::accept() Coroutine API

Date: 2026-05-10
**Status: implemented.** `tcp_accept`, `tcp_accept_multishot`, `submit_accept_borrowed`, `AcceptOp`, `MultishotAcceptOp` all landed. `str/*` bench variants landed. AC#4 is now covered by the 100-connection multishot test in `tests/socket_task_ring_test.cxx`.

## Problem

`TcpListener` in `socket_io.cxx` provides synchronous bind/listen and raw multishot
submission helpers (`arm_accept_multishot_borrowed`, `rearm_accept_multishot_borrowed`),
but `socket_io_coro.cxx` has no async counterpart. There is no way to `co_await` an
accepted connection from a coroutine.

This is a prerequisite for:

- P1-09 fair benchmark: async server variant using `SocketTaskRing`
- Future HTTP server migration off hand-rolled CQE dispatch in favour of coroutine server
  loops

## Design constraints (from existing codebase)

1. **Plain factory pattern** — not a coroutine itself; returns `Task<T>` via
   `make_task_source` + shared state machine + cancel hook. See `tcp_connect` and all
   recv helpers in `socket_io_coro.cxx`.

2. **State machine via `shared_ptr<Op : enable_shared_from_this<Op>>`** — the `Op` owns
   the `TaskSource`, drives ring submissions, dispatches CQE callbacks. Lifetime
   independent of the awaiting Task.

3. **Cancel semantics** — `TaskSource::install_cancel_hook` → `submit_on_owner` →
   cancel SQE from ring-owner thread. Cancel by user-data when no fd exists yet; cancel
   by fd once the listening fd is known (multishot).

4. **`CompletionTable::reserve_multishot`** — slot survives CQE when `CQE_F_MORE` is
   set and `res >= 0`, freeing only on terminal CQE. `reserve` (single-shot) frees on
   first CQE regardless of flags. `CompletionTable::dispatch` frees the slot **before**
   invoking the callback (uring_completion.cxx lines 103-112), so reserving a new slot
   from within a callback is safe.

5. **`SocketFdMode`** — `os_fd` is the safe default. `direct_if_available` and
   `direct_required` require slot management. Both `tcp_accept` and `tcp_accept_multishot`
   return `ENOTSUP` for `direct_required` (same guard as `tcp_connect`).

6. **`TCP_NODELAY`** — applied via `setsockopt` on the OS fd after accept. On error,
   `OwnedSocketHandle` RAII closes the accepted fd before returning.

7. **No single-shot `submit_accept` exists** in `socket_io.cxx` today — only multishot
   variants. Must add.

8. **`SocketHandle = RingFd`** (alias at `socket_io.cxx` line 26). Use `SocketHandle`
   for cancel-by-fd fields; `RingFd` is an implementation detail.

9. **`co_spawn` was removed**. Current fire-and-forget idiom is `move(task).detach()`.
   Called from a `noexcept` callback — the handler factory and `Task::detach()` must not
   throw. `bad_alloc` in handler construction terminates; this is an explicit contract.

10. **`tcp_parallel_coro_bench`** is disabled (CMakeLists.txt lines 42-44) and still
    calls `co_spawn`. It remains disabled for this PR; porting it to `.detach()` is a
    separate task.

11. **`Fn<T> = std::function<T>`** (types.cxx:47). `std::function` requires its target
    to be CopyConstructible. All existing socket_io callbacks use `Fn<>` with this same
    constraint. Handlers passed to `tcp_accept_multishot` must be copyable; move-only
    lambdas will fail to compile.

12. **`submit_accept_borrowed` must call `sqe.add_flags(listen.sqe_fd_flags())`** — every
    other submit helper does this (e.g. `submit_accept_multishot_borrowed` line 631,
    `submit_connect_borrowed` line 1042). Without it, a direct-fd `SocketHandle` passed
    as `listen` will not have the `IOSQE_FIXED_FILE` flag set and the kernel will
    misinterpret the fd number.

13. **`submit_on_ring_owner` is required for cross-thread cancel** — `socket_io.cxx:1346`:
    "Cross-thread cancel callers MUST provide `submit_on_ring_owner`." If `Task::cancel()`
    is called from a WorkPool thread (the normal case), `SocketTaskRingOptions::submit_on_ring_owner`
    must be set or `submit_on_owner` will assert and call the callback inline on the wrong
    thread.

## Scope

### In scope (this proposal)

1. **`AcceptOptions`** struct: `tcp_nodelay`, `tcp_quickack`
2. **`submit_accept_borrowed`** in `socket_io.cxx` — single-shot `IORING_OP_ACCEPT`
3. **`AcceptOp`** state machine + **`tcp_accept`** factory in `socket_io_coro.cxx`
4. **`MultishotAcceptOp`** state machine + **`tcp_accept_multishot`** factory in
   `socket_io_coro.cxx`

### Out of scope

- `direct_if_available` / `direct_required` fd mode for accepted sockets
- `cmd_sock_setsockopt` for TCP_NODELAY on direct fds
- per-accept timeout (server loops cancel by stopping the outer Task)
- `tcp_parallel_coro_bench` `.detach()` port (separate task)

## AcceptOptions

```
struct AcceptOptions {
    bool tcp_nodelay{true};
    bool tcp_quickack{false};  // non-sticky: kernel may revert quickack mode based on internal heuristics; best-effort only
};
```

No timeout field. Accept timeout belongs to the outer server loop's cancellation.

## submit_accept_borrowed (new, socket_io.cxx)

```
bool submit_accept_borrowed(
    SocketRawRing& ring,
    SocketHandle listen,
    sockaddr* addr,
    socklen_t* addrlen,
    u64 user_data,
    int accept_flags) noexcept
```

Single `prep_accept` SQE. Always OS fd (no direct variant for first pass). Returns
false if SQE unavailable. `addr`/`addrlen` may be `nullptr`; kernel fills nothing and
this is valid per `accept(2)`.

`accept_flags` is the `accept4(2)` per-socket flags (`SOCK_CLOEXEC`, `SOCK_NONBLOCK`) —
distinct from SQE flags. SQE flags are managed internally via `listen.sqe_fd_flags()`.

Must call `sqe.add_flags(listen.sqe_fd_flags())` — matches every existing submit helper
in `socket_io.cxx`. Even though this function targets OS fds, the helper accepts
`SocketHandle` which can be direct; `sqe_fd_flags()` returns the correct flag for either
mode.

## AcceptOp state machine

```
struct AcceptOp : enable_shared_from_this<AcceptOp> {
    SocketTaskRing* ring{};
    SP<TaskSource<TcpStream>> src{};
    AcceptOptions opts{};
    int accept_flags{SOCK_CLOEXEC|SOCK_NONBLOCK};   // overridden by factory from listener
    u64 accept_ud{};
    atomic_bool cancel_requested{false};
    atomic_bool finalized{false};

    [[nodiscard]] bool try_finalize() noexcept {
        return !finalized.exchange(true, memory_order_acq_rel);
    }
    void complete_exception(IoError e) noexcept {
        if (!try_finalize()) return;
        auto _ = src->try_set_exception(make_exception_ptr(move(e)));
    }
    void complete_cancelled() noexcept {
        if (!try_finalize()) return;
        auto _ = src->try_set_cancelled();
    }
    void complete_value(TcpStream v) noexcept {
        if (!try_finalize()) return;
        auto _ = src->try_set_value(Success<TcpStream>{move(v)});
    }

    void on_accept_cqe(IoResult r) noexcept;
    void cancel_on_owner(SocketTaskRing& r) noexcept;
    void request_cancel(CancelReason) noexcept;
};
```

### on_accept_cqe

```
void on_accept_cqe(IoResult r) noexcept {
    if (r.res < 0) {
        if (cancel_requested.load(memory_order_acquire))
            complete_cancelled();
        else
            complete_exception(IoError{-r.res, "tcp_accept: accept"});
        return;
    }
    auto owned = OwnedSocketHandle::from_fd(r.res);  // RAII: closes on early return
    if (cancel_requested.load(memory_order_acquire)) {
        complete_cancelled();
        return;
    }
    SocketHandle const h = owned.get();
    if (opts.tcp_nodelay && h.is_os_fd()) {
        int const one = 1;
        if (::setsockopt(h.as_fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
            complete_exception(IoError{errno, "tcp_accept: TCP_NODELAY"});
            return;  // owned closes the accepted fd
        }
    }
    if (opts.tcp_quickack && h.is_os_fd()) {
        int const one = 1;
        if (::setsockopt(h.as_fd(), IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one)) < 0) {
            complete_exception(IoError{errno, "tcp_accept: TCP_QUICKACK"});
            return;
        }
    }
    complete_value(TcpStream{make_shared<TcpStreamState>(ring, move(owned))});
}
```

`try_finalize` inside each `complete_*` means a racing cancel CQE arriving after the
accept CQE has already completed is harmless — the second writer loses.

### cancel_on_owner

Single SQE pending; cancel by user-data (no fd yet — the accepted fd arrives in CQE
`res`, not before):

```
void cancel_on_owner(SocketTaskRing& r) noexcept {
    if (finalized.load(memory_order_acquire)) return;
    auto self = shared_from_this();
    auto [cs, cg] = r.completions().reserve([self](IoResult) noexcept {});
    if (!submit_cancel_by_ud(r.raw(), accept_ud, r.encode(cs, cg))) {
        r.completions().dispatch(cs, cg, -EBUSY, 0);
        complete_cancelled();
        return;
    }
    auto _ = r.raw().submit();
}
```

### request_cancel

Same pattern as `ConnectOp::request_cancel`:

```
void request_cancel(CancelReason) noexcept {
    cancel_requested.store(true, memory_order_release);
    auto self = shared_from_this();
    if (!ring->submit_on_owner([self](SocketTaskRing& r) {
        self->cancel_on_owner(r);
    }))
        complete_cancelled();  // may run on cancelling thread; relies on TaskSource setter MT-safety
}
```

## tcp_accept factory

```
Task<TcpStream> tcp_accept(
    TcpListener& listener,
    SocketTaskRing& ring,
    AcceptOptions opts = {})
{
    if (ring.opts().fd_mode == SocketFdMode::direct_required)
        return make_error_task<TcpStream>(IoError{ENOTSUP, "tcp_accept: direct fd"});

    auto [task, raw_src] = make_task_source<TcpStream>(
        SubmitOptions{.enable_cancellation = true});
    auto src = make_shared<TaskSource<TcpStream>>(move(raw_src));

    auto op = make_shared<AcceptOp>();
    op->ring = &ring;
    op->src = src;
    op->opts = opts;
    op->accept_flags = listener.accept_flags();

    auto [slot, gen] = ring.completions().reserve(
        [op](IoResult r) noexcept { op->on_accept_cqe(r); });
    op->accept_ud = ring.encode(slot, gen);

    if (!submit_accept_borrowed(ring.raw(), listener.handle(),
                                nullptr, nullptr, op->accept_ud, op->accept_flags))
        ring.completions().dispatch(slot, gen, -ENOSPC, 0);

    auto _ = src->install_cancel_hook(
        [weak_op = WP<AcceptOp>{op}](CancelReason cr) noexcept {
            if (auto op = weak_op.lock()) op->request_cancel(cr);
        });

    return task;
}
```

SQ-full is synthesized as `-ENOSPC` into the slot; `on_accept_cqe` surfaces it as
`IoError{ENOSPC, "tcp_accept: accept"}`. This is the same pattern as `tcp_connect`
(socket SQE full → `ENOSPC`). Note: this error occurs before the kernel sees the accept,
so the message is slightly misleading — acceptable for now, same precedent in codebase.

`install_cancel_hook` is last — same ordering as `tcp_connect`. Relies on
`TaskSource::install_cancel_hook` firing the hook synchronously if the source was
already cancelled (e.g. accept CQE fired and resolved before hook installed).

## TcpListener additions

Add one getter to expose `accept_flags_`:

```
[[nodiscard]] int accept_flags() const noexcept { return accept_flags_; }
```

## Multishot design

### Lifetime contract

**`TcpListener` must outlive the `Task<void>` returned by `tcp_accept_multishot`.**
`MultishotAcceptOp` captures `listener.handle()` (a `SocketHandle` value = a raw fd
number). If `TcpListener` is destroyed (dtor closes `fd_`) while the multishot is in
flight, the cancel-by-fd targets a closed fd. Cancel and `co_await` the `Task<void>`
before destroying the listener. Do not move-assign a `TcpListener` while a multishot
op is bound to it.

### Threading and cancel preconditions

`SocketTaskRingOptions::submit_on_ring_owner` **must** be set if `Task::cancel()` can
be called from a thread other than the ring owner — which is the normal case when the
Task is awaited in a coroutine on a `WorkPool`. Without it, `submit_on_owner` asserts
caller == owner and calls the cancel closure inline on the cancelling thread, violating
the ring-owner invariant. Both `tcp_accept` and `tcp_accept_multishot` have this
requirement (same as `tcp_connect`).

### API

```
Task<void> tcp_accept_multishot(
    TcpListener& listener,
    SocketTaskRing& ring,
    AcceptOptions opts,
    Fn<Task<void>(TcpStream)> handler)
```

`handler` must be CopyConstructible (`Fn<T> = std::function<T>`; move-only lambdas will
fail to compile). `handler` is invoked on the **ring-owner thread** (from within
`CompletionTable::dispatch`). `handler(stream)` must not throw — if constructing the
returned `Task<void>` throws (e.g. `bad_alloc`), `std::terminate` is called because
`on_accept_cqe` is `noexcept`. Whether the handler body runs on the ring-owner thread
depends on what it `co_await`s; typically it suspends immediately at the first `co_await`
and resumes on a `WorkPool` thread.

The outer `Task<void>` runs until cancelled or an unrecoverable accept error occurs.
Cancelling stops new accepts; in-flight detached handler Tasks are not cancelled.
Kernel CQEs arriving between the cancel SQE and the cancel CQE may deliver additional
accepted fds — these are closed cleanly by `OwnedSocketHandle` RAII (no fd leak, but
the connections are dropped).

TCP_NODELAY/TCP_QUICKACK failures in multishot are **best-effort and silently ignored**
(swallowed). The connection is still passed to `handler`. This asymmetry with single-shot
(which errors on `setsockopt` failure) is intentional: terminating the entire accept
loop for a non-fatal per-connection option is disproportionate.

### MultishotAcceptOp state machine

```
struct MultishotAcceptOp : enable_shared_from_this<MultishotAcceptOp> {
    SocketTaskRing* ring{};
    SP<TaskSource<void>> src{};
    AcceptOptions opts{};
    int accept_flags{SOCK_CLOEXEC|SOCK_NONBLOCK};
    SocketHandle listen_fd{};   // for cancel-by-fd; caller must keep TcpListener alive
    u64 accept_ud{};
    Fn<Task<void>(TcpStream)> handler{};

    atomic_bool cancel_requested{false};
    atomic_bool finalized{false};

    [[nodiscard]] bool try_finalize() noexcept {
        return !finalized.exchange(true, memory_order_acq_rel);
    }
    void complete_exception(IoError e) noexcept {
        if (!try_finalize()) return;
        auto _ = src->try_set_exception(make_exception_ptr(move(e)));
    }
    void complete_cancelled() noexcept {
        if (!try_finalize()) return;
        auto _ = src->try_set_cancelled();
    }

    void on_accept_cqe(IoResult r) noexcept;
    void cancel_on_owner(SocketTaskRing& r) noexcept;
    void request_cancel(CancelReason) noexcept;
};
```

### on_accept_cqe

`CompletionTable::dispatch` moves out `s.fn` and frees the slot *before* invoking the
callback (uring_completion.cxx:103-112). A new `reserve_multishot` from within the
callback is therefore safe — even if `slots_` reallocates, the freed-slot reference is
never touched after `fn` is invoked. This property is load-bearing for the rearm path.

```
void on_accept_cqe(IoResult r) noexcept {
    bool const more = (r.flags & IORING_CQE_F_MORE) != 0;

    if (r.res < 0) {
        // Terminal — slot already freed by dispatch before we were called.
        if (cancel_requested.load(memory_order_acquire) || r.res == -ECANCELED)
            complete_cancelled();
        else
            complete_exception(IoError{-r.res, "tcp_accept_multishot: accept"});
        return;
    }

    auto owned = OwnedSocketHandle::from_fd(r.res);
    if (cancel_requested.load(memory_order_acquire)) {
        complete_cancelled();
        return;  // owned closes the accepted fd
    }

    // TCP_NODELAY/TCP_QUICKACK: best-effort, silently ignore failures
    if (opts.tcp_nodelay && owned.get().is_os_fd()) {
        int const one = 1;
        ::setsockopt(owned.get().as_fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    if (opts.tcp_quickack && owned.get().is_os_fd()) {
        int const one = 1;
        ::setsockopt(owned.get().as_fd(), IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
    }

    handler(TcpStream{make_shared<TcpStreamState>(ring, move(owned))}).detach();

    if (!more) {
        // Kernel disarmed multishot (SQ pressure or resource exhaustion).
        // Check cancel_requested before rearming — a cancel SQE may already be in flight;
        // rearming would create a new multishot that the pending cancel-by-fd will also
        // cancel, but it is wasteful and extends the shutdown window unnecessarily.
        if (cancel_requested.load(memory_order_acquire)) {
            complete_cancelled();
            return;
        }
        // Slot was freed before this callback; reserve a fresh multishot slot.
        // accept_ud is mutated here; only touched on ring-owner thread (no concurrent access).
        auto [slot, gen] = ring->completions().reserve_multishot(
            [self = shared_from_this()](IoResult r2) noexcept { self->on_accept_cqe(r2); });
        accept_ud = ring->encode(slot, gen);
        if (!submit_accept_borrowed(ring->raw(), listen_fd, nullptr, nullptr,
                                    accept_ud, accept_flags))
            ring->completions().dispatch(slot, gen, -ENOSPC, 0);
        // Rearm failure: synthesized -ENOSPC CQE fires on_accept_cqe synchronously
        // (via dispatch above), takes res<0 branch, calls complete_exception. The
        // re-entry depth is bounded at 2. Caller must handle IoError{ENOSPC} and restart
        // if desired.
    }
    // more == true: slot stays alive; next accept CQE fires on_accept_cqe again.
}
```

### cancel_on_owner

Cancel by fd (same mechanism as `submit_cancel_multishot_recv`). `accept_ud` is not
used for cancel; `listen_fd` is stable across rearms, so cancel-by-fd is always correct
regardless of which slot generation is current.

**Constraint**: `submit_cancel_fd` cancels *all* pending SQEs targeting `listen_fd`
(`IORING_ASYNC_CANCEL_FD` semantics). While `tcp_accept_multishot` is bound to a
`TcpListener`, no other io_uring op may target the same listener fd or it will be
cancelled on shutdown.

```
void cancel_on_owner(SocketTaskRing& r) noexcept {
    if (finalized.load(memory_order_acquire)) return;
    auto self = shared_from_this();
    auto [cs, cg] = r.completions().reserve([self](IoResult) noexcept {});
    if (!submit_cancel_fd(r.raw(), listen_fd, r.encode(cs, cg))) {
        r.completions().dispatch(cs, cg, -EBUSY, 0);
        complete_cancelled();
        return;
    }
    auto _ = r.raw().submit();
}
```

`request_cancel` mirrors `AcceptOp::request_cancel` (same pattern as `ConnectOp`) — if
`submit_on_owner` returns false, calls `complete_cancelled()` on the cancelling thread;
relies on `TaskSource::try_set_cancelled()` MT-safety.

### tcp_accept_multishot factory

```
Task<void> tcp_accept_multishot(
    TcpListener& listener,
    SocketTaskRing& ring,
    AcceptOptions opts,
    Fn<Task<void>(TcpStream)> handler)
{
    if (ring.opts().fd_mode == SocketFdMode::direct_required)
        return make_error_task<void>(IoError{ENOTSUP, "tcp_accept_multishot: direct fd"});
    if (!handler)
        return make_error_task<void>(IoError{EINVAL, "tcp_accept_multishot: empty handler"});

    auto [task, raw_src] = make_task_source<void>(
        SubmitOptions{.enable_cancellation = true});
    auto src = make_shared<TaskSource<void>>(move(raw_src));

    auto op = make_shared<MultishotAcceptOp>();
    op->ring = &ring;
    op->src = src;
    op->opts = opts;
    op->accept_flags = listener.accept_flags();
    op->listen_fd = listener.handle();
    op->handler = move(handler);

    auto [slot, gen] = ring.completions().reserve_multishot(
        [op](IoResult r) noexcept { op->on_accept_cqe(r); });
    op->accept_ud = ring.encode(slot, gen);

    if (!submit_accept_borrowed(ring.raw(), listener.handle(),
                                nullptr, nullptr, op->accept_ud, op->accept_flags))
        ring.completions().dispatch(slot, gen, -ENOSPC, 0);

    auto _ = src->install_cancel_hook(
        [weak_op = WP<MultishotAcceptOp>{op}](CancelReason cr) noexcept {
            if (auto op = weak_op.lock()) op->request_cancel(cr);
        });

    return task;
}
```

## Files touched

| File | Change |
|---|---|
| `src/socket_io/socket_io.cxx` | add `submit_accept_borrowed` (single-shot); add `accept_flags()` getter to `TcpListener` |
| `src/socket_io/socket_io.cxx` | add `AcceptOptions` (alongside `ConnectOptions` at line 1326 for symmetry) |
| `src/socket_io/socket_io_coro.cxx` | add `AcceptOp`, `tcp_accept`; `MultishotAcceptOp`, `tcp_accept_multishot` |
| `benchmarks/tcp_increment_coro_bench.cxx` | add `str/*` variants using `tcp_accept` + async server loop |
| `benchmarks/CMakeLists.txt` | add `conflux_socket_io` to `conflux_tcp_increment_coro_bench` link deps (currently only has `conflux_file_io`) |

No changes to `socket_io.hxx`, `TcpListenerOptions`, `ConnectOptions`, or any existing
API. `tcp_parallel_coro_bench` remains disabled.

## Acceptance criteria

1. Compiles and links in both debug-clang-libcxx and debug-gcc-stdcxx.
2. `str/callback` and `str/coroutine` (client) variants in `tcp_increment_coro_bench`
   produce correct output (no crash, correct increment sequence).
3. `--compare-bins` gate: `str/coroutine` within ±2% of `fr/coroutine` on release build
   (client variants; same threshold as P1-09 proposal).
4. `tcp_accept_multishot` end-to-end: a test or bench server variant exercises ≥100
   sequential connections via `tcp_accept_multishot`, verifies all connections handled
   correctly, `CompletionTable::pending()` returns to baseline after shutdown, and ASAN
   reports no errors. (Note: fd-leak detection requires counting `/proc/self/fd` entries
   before and after — ASAN does not detect fd leaks.)
5. Cancellation (single-shot): cancel outer Task while blocked in `tcp_accept` → task
   resolves cancelled. fd-count via `/proc/self/fd` returns to pre-accept baseline
   (verifies no fd leak from accepted-but-dropped connections).
6. Cancellation (multishot): cancel outer Task → `submit_cancel_fd` → terminal CQE with
   `-ECANCELED` → `complete_cancelled()`. Any fds in CQEs arriving after cancel are
   closed by `OwnedSocketHandle` RAII — no leak, connection dropped cleanly.
7. `TcpListener` destroyed after `Task<void>` awaited and resolved → no use-after-free
   (ASAN clean).

## Open questions

1. **Peer address** — `nullptr`/`nullptr` for addr/addrlen is valid (kernel writes nothing).
   If peer IP is needed later, use `getpeername` on the accepted fd post-CQE. Do NOT
   use a shared `sockaddr_storage` buffer with multishot accept: the kernel can queue
   multiple accept CQEs before userspace dispatches any, each overwriting the same buffer.
   A single stable buffer only works reliably for single-shot accept.

2. **Rearm failure in multishot** — if `submit_accept_borrowed` fails on the rearm path
   (`-ENOSPC`), the synthesized error CQE fires `on_accept_cqe(-ENOSPC)` → terminal →
   `complete_exception`. Server loop must handle this and restart. For bench use this is
   fine; a production server should wrap the outer loop with restart logic.
