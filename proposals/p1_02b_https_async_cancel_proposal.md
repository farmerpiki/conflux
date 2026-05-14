# P1-02b: HTTP/HTTPS Async Cancellation

Date: 2026-05-10
Status: IMPLEMENTED
Effort: 3–5 days (includes connect-stage cancellation)
Prerequisite: P1-02 (async HTTPS via TcpTlsStream, complete)

## Problem

`TcpTlsStream` operations (handshake, read, write, close) are
deadline-based only — they do not respond to task cancellation. When
a parent HTTP request task is cancelled, TLS operations continue
running until their timeout expires.

```
HTTP request cancelled
  → TlsStreamRef.recv() NOT cancelled
    → TcpTlsStream::read_some() NOT cancelled
      → fill_rbio(deadline) eventually times out
        → Timeout error returned (not CancellationError)
```

The socket layer (`TcpStream`) is fully cancellation-aware:
`recv_borrowed`, `write_borrowed`, and `close` all install cancel hooks.
The gap is in the TLS wrapper layer. Plain HTTP has the same
parent-to-child cancellation gap.

Implementation marker: `client_async_impl.cxx` threads `ActiveTaskCancelRelay` through connect/TLS operations; `tls.cxx` accepts the relay and awaits cancellable socket children. The old `// TODO(P1-02): TLS cancel not wired` marker is gone.

## Current State

Implemented. `ActiveTaskCancelRelay` is a standalone `conflux.net.cancel` module, `HappyConnectState` keeps `pending` and cancels active attempts without calling cancellation while holding its mutex, and `TcpTlsStream` receives the relay so handshake/read/write await cancellable socket children.

| Operation | TcpStream | TcpTlsStream | Gap |
|---|---|---|---|
| recv | `enable_cancellation=true` + cancel hook | Relay-aware child recv | Closed |
| write | `enable_cancellation=true` + cancel hook | Relay-aware child write | Closed |
| handshake | N/A | Relay-aware socket children | Closed |
| close | Cancel hook / shielded close path | Timeout close path retained | Acceptable close semantics |

## Design History

### V1 (rejected): Flag-only cancellation

Added `Atom<bool> cancel_requested_` to `TcpTlsStream`, checked between
TLS loop iterations. **Rejected** — coroutine is usually suspended inside
`recv_borrowed()` / `write_borrowed()`, so flag is only checked after child
op completes. Cannot meet `< 50 ms` cancellation.

Additional V1 problems:
- `drain_wbio_*()` uses `write_all_borrowed()` which wraps `write_borrowed()` without propagating cancellation to inner op
- `install_cancel_hook()` on coroutine body doesn't match `work.root` API (hooks go on `TaskSource`)
- `CancelledError{}` won't compile — requires `CancelReason`
- `Atom<bool>` breaks `TcpTlsStream` move support

### V2 (revised): Shared cancellation relay + `TaskControl` hook

Replaced flag with `RequestCancelState` that tracks and cancels active
child `TaskControl`. **Partially accepted** — core relay design correct,
but integration shape wrong: proposed adding `install_cancel_hook()` to
`TaskControl`, which doesn't match codebase patterns.

V2 also missed post-completion cancellation check in `await_child()`:
child completes → cancel arrives before parent resumes → `await_child()`
returns success → caller continues even though cancelled. With io_uring
completion + cancel racing, this is a normal edge case.

### V3 (revised): TaskSource/driver + post-completion check

Fixed integration to use `TaskSource` + detached driver (matches DNS
resolver pattern). Added `throw_if_cancelled()` after child completion
in `await_child()`. **Mostly accepted** — four remaining issues:

1. Plain HTTP `write()` still wraps `write_all_borrowed()` → cancelling wrapper won't cancel leaf `write_borrowed()`
2. Connect-stage cancellation underspecified — Happy Eyeballs `staggered_parallel_connect()` not cancellable
3. `CancelledError` can be caught by broad `catch (...)` in connect stage → becomes `HttpError{connect}` instead of cancelled task
4. `stream_.close()` must not go through `await_child()` — relay already cancelled → would cancel close task

### V4 (revised): PlainStreamRef + HappyConnectState + rename

Added cancel-aware `PlainStreamRef`, `HappyConnectState` with
multi-attempt tracking, `CancelledError` re-throw, direct close.
**Mostly accepted** — four remaining issues:

1. `ActiveTaskCancelRelay` must live in standalone module, not `tls.cxx` — plain HTTP needs it even when `CONFLUX_HAS_TLS=0`
2. `HappyConnectState::register_attempt()` calls `request_cancel()` under mutex — must copy control out first
3. `HappyConnectState` drops existing `pending` field needed for "all attempts failed" detection
4. Winner `TaskSource` in `staggered_parallel_connect()` must be cancellation-enabled and terminaled immediately on cancel, otherwise awaiter waits until attempts finish

## Design (V5 — Final)

### ActiveTaskCancelRelay — Standalone Module

Lives in `conflux.net.cancel`, not `tls.cxx`. Plain HTTP needs it
even when `CONFLUX_HAS_TLS=0`.

```cpp
export module conflux.net.cancel;

import std;
import conflux.types;
import conflux.work;
```

```cpp
export struct ActiveTaskCancelRelay {
    mutex m;
    optional<wroot::TaskControl> active;
    Atom<bool> cancelled{false};

    void set_active(wroot::TaskControl c) {
        optional<wroot::TaskControl> to_cancel;
        {
            scoped_lock lk{m};
            active.emplace(move(c));
            if (cancelled.load(memory_order_acquire))
                to_cancel = active;
        }
        if (to_cancel)
            (void)to_cancel->request_cancel();
    }

    void clear_active() noexcept {
        try {
            scoped_lock lk{m};
            active.reset();
        } catch (...) {}
    }

    void cancel() noexcept {
        optional<wroot::TaskControl> to_cancel;
        {
            scoped_lock lk{m};
            cancelled.store(true, memory_order_release);
            to_cancel = active;
        }
        if (to_cancel)
            (void)to_cancel->request_cancel();
    }

    void throw_if_cancelled() const {
        if (cancelled.load(memory_order_acquire))
            throw wroot::CancelledError{wroot::CancelReason::requested};
    }

    template<class T>
    wroot::Task<T> await_child(wroot::Task<T> child) {
        set_active(child.control());

        try {
            if constexpr (is_void_v<T>) {
                co_await move(child);
                clear_active();
                throw_if_cancelled();
                co_return;
            } else {
                auto out = co_await move(child);
                clear_active();
                throw_if_cancelled();
                co_return out;
            }
        } catch (...) {
            clear_active();
            throw;
        }
    }
};
```

### PlainStreamRef — Cancel-Aware

Plain HTTP write loops over `write_borrowed()` directly, not
`write_all_borrowed()`:

```cpp
struct PlainStreamRef {
    TcpStream& s;
    SP<ActiveTaskCancelRelay> cancel;
    chrono::milliseconds per_recv;
    chrono::milliseconds per_write;

    [[nodiscard]] wroot::Task<SZ> recv(span<u8> buf) {
        auto child = per_recv.count() <= 0
            ? s.recv_borrowed(buf)
            : s.recv_borrowed(buf, per_recv);
        return cancel->await_child(move(child));
    }

    [[nodiscard]] wroot::Task<SZ> recv(span<u8> buf, chrono::milliseconds t) {
        auto child = t.count() <= 0
            ? s.recv_borrowed(buf)
            : s.recv_borrowed(buf, t);
        return cancel->await_child(move(child));
    }

    [[nodiscard]] wroot::Task<void> write(span<u8 const> buf) {
        SZ sent = 0;
        while (sent < buf.size()) {
            cancel->throw_if_cancelled();

            auto child = s.write_borrowed(
                span<u8 const>{buf.data() + sent, buf.size() - sent},
                per_write);

            SZ const n = co_await cancel->await_child(move(child));
            if (n == 0)
                throw IoError{ECONNRESET, "tcp: connection closed"};

            sent += n;
        }
    }
};
```

### TcpTlsStream Integration

`TcpTlsStream` accepts optional cancel state. Default-constructs one
for standalone use:

```cpp
TcpTlsStream(TlsContext& ctx, TcpStream stream,
             SP<ActiveTaskCancelRelay> cancel = make_shared<ActiveTaskCancelRelay>());
```

All leaf socket I/O goes through `await_child()`:

```cpp
// fill_rbio
auto task = stream_.recv_borrowed(span<u8>{scratch_}, remaining);
auto const got = co_await cancel_->await_child(move(task));
```

`drain_wbio_*()` loops over `write_borrowed()` directly (drops
`write_all_borrowed()`):

```cpp
while (off < static_cast<SZ>(got)) {
    cancel_->throw_if_cancelled();

    auto const now = chrono::steady_clock::now();
    if (now >= deadline)
        throw IoError{ETIMEDOUT, "tcp: send timed out"};

    auto remaining = chrono::ceil<ms>(deadline - now);

    auto child = stream_.write_borrowed(
        span<u8 const>{scratch_.data() + off, static_cast<SZ>(got) - off},
        remaining);

    SZ const n = co_await cancel_->await_child(move(child));
    if (n == 0)
        throw IoError{ECONNRESET, "tcp: connection closed"};

    off += n;
}
```

### Connect-Stage Cancellation

`staggered_parallel_connect()` launches detached `happy_attempt()` tasks.
Single-active-task relay cannot cancel concurrent attempts. Extend with
multi-attempt tracking.

**Winner source must be cancellation-enabled** — `request_cancel()` only
requests; it does not complete the task. Hook must terminal the source
immediately so the awaiter wakes promptly:

```cpp
auto [task, raw_src] = wroot::make_task_source<TcpStream>(
    wroot::SubmitOptions{.enable_cancellation = true});

auto winner_src = make_shared<wroot::TaskSource<TcpStream>>(move(raw_src));

auto _ = winner_src->install_cancel_hook(
    [hs, winner_src](wroot::CancelReason) noexcept {
        hs->cancel_all();
        auto _ = winner_src->try_set_cancelled();
    });
```

`HappyConnectState` keeps existing `pending` field. Never calls
`request_cancel()` under mutex — copies control out first:

```cpp
struct HappyConnectState {
    Atom<bool> won{false};
    Atom<bool> fast_fail{false};
    Atom<bool> cancelled{false};
    Atom<int> pending{0};
    mutex m;
    V<wroot::TaskControl> attempts;

    void register_attempt(wroot::TaskControl c) {
        optional<wroot::TaskControl> cancel_now;
        {
            scoped_lock lk{m};
            if (cancelled.load(memory_order_acquire))
                cancel_now = c;
            else
                attempts.push_back(move(c));
        }
        if (cancel_now)
            (void)cancel_now->request_cancel();
    }

    void cancel_all() noexcept {
        V<wroot::TaskControl> copy;
        {
            scoped_lock lk{m};
            cancelled.store(true, memory_order_release);
            copy = attempts;
        }
        for (auto& c : copy)
            (void)c.request_cancel();
    }
};
```

Each `happy_attempt()` creates task, registers control, then awaits:

```cpp
auto connect_task = tcp_connect(ring, fam, ss, addr_len, copts);
hs->register_attempt(connect_task.control());
auto s = co_await move(connect_task);
```

`happy_attempt()` catches `CancelledError` separately — cancellation
must not set `fast_fail=true` or masquerade as "all endpoints failed":

```cpp
catch (wroot::CancelledError const&) {
    // cancelled, not failed — do not set fast_fail
}
```

Stagger loop stops launching new attempts once `hs->cancelled == true`.

When `cancelled == true` and `pending` reaches zero, do **not** set
`ECONNREFUSED` on winner source — it is already terminal (cancelled).

### Integration in send_async — TaskSource/Driver Pattern

Use existing `TaskSource` + detached driver pattern (matches DNS resolver
cancellation model). Do **not** add `install_cancel_hook()` to `TaskControl`.

```cpp
wroot::Task<HttpResult> send_async(
    HttpClient const& client,
    SocketTaskRing& ring,
    HttpRequest const& req)
{
    auto [out, raw_src] = wroot::make_task_source<HttpResult>(
        wroot::SubmitOptions{.enable_cancellation = true});

    auto src = make_shared<wroot::TaskSource<HttpResult>>(move(raw_src));
    auto cancel = make_shared<ActiveTaskCancelRelay>();

    auto _ = src->install_cancel_hook([cancel](wroot::CancelReason) noexcept {
        cancel->cancel();
    });

    auto driver = async_detail::run_async_request_driver(
        ring,
        req,
        client.options(),
        src,
        cancel);

    move(driver).detach();
    return move(out);
}
```

Driver bridges `do_async_request` result into `TaskSource`:

```cpp
try {
    auto result = co_await do_async_request(..., cancel);
    auto _ = src->try_set_value(wroot::Success<HttpResult>{move(result)});
} catch (wroot::CancelledError const&) {
    auto _ = src->try_set_cancelled();
} catch (...) {
    auto _ = src->try_set_exception(current_exception());
}
```

### CancelledError Re-throw Before Broad Catches

In `do_async_request()`, connect stage has `catch (...)` after
`catch (IoError const&)`. Once cancellation can escape from connect:

```cpp
catch (wroot::CancelledError const&) {
    throw;
}
```

before broad catches, otherwise cancellation becomes
`HttpError{connect}` instead of a cancelled task.

### Cancellation Checkpoints

Add `cancel->throw_if_cancelled()` at these synchronous boundaries:

- Before DNS (`getaddrinfo()` is blocking, not cancellable — documented limitation)
- After DNS
- Before connect
- After connect
- Before TLS handshake
- Before request write
- Before body read

### Close Behavior

Normal TLS read/write/handshake: cancellation throws `CancelledError`.

Close path: do **not** let cancellation skip `stream_.close()`.
Call `stream_.close()` directly, **not** through `await_child()` —
relay is already cancelled and would cancel the close task:

```cpp
// normal ops: throw on cancel
co_await drain_wbio_until(deadline, cancel_mode::throw_cancelled);

// close path: skip TLS close_notify drain but still close fd
co_await drain_wbio_for(timeout, cancel_mode::return_early);
co_await stream_.close();  // direct call, not through relay
```

Without explicit `SO_LINGER`, peer may see FIN, EOF without TLS
`close_notify`, or reset depending on kernel/socket state. Important
guarantee is local fd/resource release, not specific peer-visible TCP event.

## Changes

| File | Change |
|---|---|
| New `cancel.cxx` module (`conflux.net.cancel`) | `ActiveTaskCancelRelay` — standalone, no TLS dependency |
| `client_async.cxx` | `PlainStreamRef` wraps leaf `write_borrowed()` directly via relay |
| `client_async.cxx` | `send_async()` → `TaskSource` + detached driver + cancel relay |
| `client_async.cxx` | `CancelledError` re-thrown before broad `catch (...)` in connect |
| `client_async.cxx` | Cancellation checkpoints at sync boundaries |
| `client_async.cxx` | `HappyConnectState` extended: `cancelled`, `pending`, `cancel_all()`, `register_attempt()` |
| `client_async.cxx` | `staggered_parallel_connect()` winner source cancellation-enabled, terminaled on cancel |
| `client_async.cxx` | `happy_attempt()` catches `CancelledError` separately, registers `tcp_connect().control()` |
| `client_async.cxx` | Stagger loop stops launching attempts when `cancelled == true` |
| `tls.cxx` | `TcpTlsStream` ctor accepts optional `SP<ActiveTaskCancelRelay>` |
| `tls.cxx` | All leaf I/O routed through `cancel_->await_child()` |
| `tls.cxx` | `drain_wbio_*()` loops over `write_borrowed()` directly |
| `tls.cxx` | `drain_wbio_*()` accepts `cancel_mode` for close vs normal |
| `tls.cxx` | Close calls `stream_.close()` directly, not through relay |
| `tls.cxx` | Remove `// TODO(P1-02): TLS cancel not wired` |

## Acceptance Criteria

1. Cancelling parent `send_async()` cancels active child socket op for **both HTTP and HTTPS**.
2. `CancelledError` propagated to caller, not `TimeoutError` or `HttpError`.
3. Clean socket close after cancel — no fd leak.
4. `stream_.close()` always called even when cancelled during TLS close, via direct call not relay.
5. No regression in existing HTTP client tests.
6. Cancelling during `fill_rbio()` interrupts underlying `recv_borrowed()`, not just next TLS loop.
7. Cancelling during `drain_wbio_*()` interrupts active `write_borrowed()`.
8. Post-completion cancellation check: if cancel arrives between child completion and parent resume, `CancelledError` thrown (not stale success).
9. Cancelling during connect cancels active `tcp_connect()` attempts in Happy Eyeballs; winner source terminaled immediately.
10. Cancelling during blocking DNS does **not** guarantee `< 50 ms` (documented limitation); cancellation checked before and after DNS.
11. Plain HTTP writes go through leaf `write_borrowed()` via relay, not `write_all_borrowed()`.
12. `CancelledError` in `happy_attempt()` does not set `fast_fail` or masquerade as connection failure.
13. Regression test: timeout large, cancellation after task suspended in read/write, completion still prompt (< 50 ms post-connect).

## Implementation Notes

1. `ActiveTaskCancelRelay` lives in standalone `conflux.net.cancel` module — no TLS dependency.
2. `staggered_parallel_connect()` winner source is cancellation-enabled and terminaled immediately on cancel.
3. `HappyConnectState` keeps existing `pending` field.
4. `happy_attempt()` treats `CancelledError` separately from connection failures.
5. No `request_cancel()` calls while holding `HappyConnectState::m` — copy control out first.

## Not in Scope

- TLS session resumption (reduces handshake cost, separate concern).
- Server-side TLS cancellation (`TlsAsyncStream` in tls.cxx uses FileReader,
  not TcpStream; different cancel mechanism).
- Prompt cancellation during blocking `getaddrinfo()` (requires async DNS, separate work).
