# HTTP Deferred Task Cancellation Integration

## Verdict

Implement a bounded P0 integration for HTTP deferred and async handler task
ownership. This patch is behavioral, not architectural.

HTTP should own connection, request, stream, and borrowed-storage lifetimes. The
root task layer should provide cancellation propagation, cancellation reasons,
and ready completion primitives. Do not rewrite the server CQE/read/write loop
around generic `race()` as part of this work.

## Scope

Implement only:

- HTTP deferred and async handler task ownership.
- Reasonful cancellation.
- Nonblocking ready completion.
- Disconnect/drop cancellation.
- Route timeout installed before async task completion.

Defer:

- Public or public-ish `RequestLifetime` redesign.
- Protocol timeout taxonomy.
- Generic HTTP race wrappers.
- Drain registry and metrics expansion.
- `ActiveTaskCancelRelay` removal outside the route/deferred server handler path.

## First Patch Contract

### 1. `DeferredResponse` Owns Cancellation Semantics

Add explicit methods, or equivalent internal hooks:

```cpp
cancel_deadline();   // CancelReason::deadline
cancel_disconnect(); // CancelReason::requested for now
cancel_shutdown();   // CancelReason::shutdown
```

Behavior:

- Deadline expiry completes with 504 Gateway Timeout.
- The attached task receives `CancelReason::deadline`.
- Disconnect/drop requests cancellation before the handle is reset.
- Shutdown/drain requests cancellation with `CancelReason::shutdown`.
- No new HTTP-local relay abstraction is introduced.

### 2. Replace Blocking Ready Callback Path

Current shape to remove:

```cpp
set_on_ready_or_run(...);
blocking_join(...);
```

Target shape:

- Install an exclusive ready callback.
- The callback only completes or marks the deferred response.
- Outcome extraction happens only after the task is known ready.
- No blocking join runs inside the ready callback.
- A task that already has a ready callback is rejected or handled explicitly.

If root lacks a clean public or internal helper for this, add a narrow root
helper instead of hiding `blocking_join` inside HTTP.

### 3. Route Timeout Enters Defer Construction

Preferred shape:

```cpp
router_defer_http_task(
    std::move(task),
    DeferredTaskOptions{.timeout = route_timeout});
```

or:

```cpp
router_defer_http_task(std::move(task), route_timeout);
```

Do not wait for a `Response` to exist. For async routes, timeout must be
installed when the task is converted into a deferred response.

### 4. `conn_erase()` Cancels Before Reset

Before dropping `deferred_response`:

```text
if active deferred task exists:
  request_cancel(requested)
  keep storage/handle alive until safe terminal handling path
then erase/reset connection state
```

Invariant: no handler may keep borrowed request, body, form, file, or header
storage past destruction unless HTTP still owns enough state for terminal
cleanup.

### 5. Keep `ActiveTaskCancelRelay`

Stop adding new HTTP-local relays. Migrate the route/deferred server handler
path first. Leave TLS and client uses untouched.

## Acceptance Tests

P0 tests:

- Async route timeout returns 504.
- Handler observes `CancelReason::deadline`.
- Client disconnect cancels the active handler task.
- Shutdown/drain cancellation reason is observable as `shutdown`.
- Ready callback path does not block.
- Task already having a ready callback is rejected or handled explicitly.
- Borrowed request storage survives until the cancelled handler reaches terminal
  completion.

## Non-Goals

- Do not expose `leave_running` for HTTP request tasks.
- Do not put generic `race()` into every recv/send path.
- Do not rely on reasonless `request_cancel()` for deadline, shutdown, or route
  timeout behavior.
- Do not make `race` a curated HTTP API.

## Bottom Line

Make deferred handler cancellation boring first. Once deferred handler ownership,
deadline cancellation, disconnect cancellation, and nonblocking ready completion
are proven, build `RequestContext` and extractor ergonomics on top of those
rules.

## Implementation Status

Achieved in the first implementation pass:

- `DeferredResponse` exposes reasonful cancellation hooks:
  `cancel_deadline()`, `cancel_disconnect()`, and `cancel_shutdown()`.
- Deferred deadline expiry completes 504 and requests
  `CancelReason::deadline` on the attached task.
- Deferred task completion no longer uses `blocking_join()` in the ready
  callback; it registers with `try_set_on_ready()` and extracts with
  `join_ready()` only after readiness.
- Already-installed ready callbacks are handled explicitly by completing the
  deferred response with an internal error and requesting cancellation.
- Context async route timeout is carried into defer construction through
  `DeferredTaskOptions`.
- `App` context/async extracted routes pass their route timeout metadata into
  the router context route table.
- HTTP/1 connection erase cancels active deferred response work with
  `CancelReason::requested`.
- H2 deferred wait clearing cancels deferred work with
  `CancelReason::requested`.
- Shutdown/immediate drain close paths request `CancelReason::shutdown` for
  active deferred response work.
- Tests cover reasonful `DeferredResponse` cancellation, deadline hook
  propagation, cancellable wrapper forwarding, borrowed async request storage,
  and async context route timeout returning 504 while the handler observes
  `CancelReason::deadline`.

Deferred after this pass:

- Public `RequestLifetime` or extractor ergonomics.
- Protocol timeout taxonomy.
- Generic HTTP race wrappers.
- Drain registry and metrics expansion.
- `ActiveTaskCancelRelay` removal outside the route/deferred server handler
  path.
- Full cancellation propagation through every App-generated coroutine wrapper;
  the first pass preserves existing borrowed-storage behavior and proves the
  direct context/deferred route path.
