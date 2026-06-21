# HTTP Async Client Ergonomics Plan

## Problem

`conflux::http::async_send(HttpClient const&, SocketTaskRing&, ClientRequest const&)`
is a real async HTTP client transport, but using it outside the HTTP server
requires callers to assemble raw io_uring plumbing: ring allocation,
`CompletionTable`, user-data encoding, `SocketTaskRing`, and a pump loop.

That low-level API should remain available for advanced users. The friction is
that ordinary callers have no supported path between `blocking_send(...)` and
full manual `SocketTaskRing` ownership.

## Current Contracts To Preserve

- `HttpClient::blocking_send(...)` returns `ClientResult`.
- `async_send(...)` returns `conflux::work::root::Task<ClientResult>`.
- `ClientResult` is the public HTTP client error model; this plan does not add a
  throwing wrapper.
- `ClientRequest` already carries phase timeouts through `HttpTimeouts`.
- `async_send(..., SocketTaskRing&, ClientRequest const&)` keeps its current
  low-level contract: `client`, `ring`, and `request` must outlive the coroutine.
- `SocketTaskRing` has owner-thread rules. Cross-thread cancellation requires a
  valid `SocketTaskRingOptions::submit_on_ring_owner` implementation.
- `http_client` and `http_async_client` are separate components today.

## Phase 1: One-Shot Async-Backed Send

Add a convenience helper for callers that want the async client runtime without
manual ring setup:

```cpp
namespace conflux::http {

struct AsyncClientRunOptions {
  unsigned ring_entries = 256;
};

[[nodiscard]] ClientResult async_blocking_send(
  HttpClient const& client,
  ClientRequest const& request,
  AsyncClientRunOptions opts = {});

[[nodiscard]] ClientResult async_blocking_send(
  HttpClient const& client,
  ClientRequest&& request,
  AsyncClientRunOptions opts = {});

} // namespace conflux::http
```

Naming is deliberately explicit: this helper runs the async transport, but it
blocks the caller while it owns and pumps a temporary socket ring.

The overload set must support the common calls without casts:

```cpp
auto r1 = async_blocking_send(client, ClientRequest::get(url));
auto req = ClientRequest::get(url).build();
auto r2 = async_blocking_send(client, req);
auto r3 = async_blocking_send(client, std::move(req));
```

### Phase 1 Implementation

1. Add a small production helper, likely in the `conflux_net_async_client`
   target, that owns:
   - `::io_uring`
   - `conflux::uring::CompletionTable`
   - `conflux::socket_io::SocketTaskRing`
2. Use the same user-data encoding already used by low-level tests:
   low 32 bits = completion slot, high 32 bits = generation.
3. Do not write a new pump loop if the existing
   `conflux::socket_io::sync_wait_socket_task(...)` is sufficient. The helper
   should construct the ring, call `async_send(client, task_ring, owned_request)`,
   then run it through `sync_wait_socket_task`.
4. Store a local owned `ClientRequest` before creating the task for every
   overload, so the `ClientRequest const&` required by `async_send(...)` remains
   alive until `sync_wait_socket_task(...)` returns.
5. Do not add a separate helper-level timeout in Phase 1. Request-level HTTP
   phase deadlines continue to come from `ClientRequest::timeouts()`. A future
   helper-level pump budget can be added only if it maps cleanly to
   `ClientResult` and does not leave socket work running after return.
6. `async_blocking_send` must catch exceptions from
   `sync_wait_socket_task(...)` and convert expected runtime failures into
   `ClientResult` errors. `conflux::work::Cancelled` maps to a documented
   `HttpError`; pump/runtime failures map to `HttpErrorKind::protocol` with a
   clear message. Only programmer errors may throw, and docs must say so.
7. If `io_uring_queue_init` fails, return `ClientResult` with
   `HttpErrorKind::protocol`, `phase = HttpPhase::connect`, `os_errno = -rc`,
   and a message naming `io_uring_queue_init`. Do not throw for normal
   initialization failure.

### Phase 1 Public Surface

Place the helper with the async transport, not the blocking-only client:

- implementation: `conflux_net_async_client`
- module: `conflux.net.async_client`
- component: `http_async_client`
- docs: `docs/conflux-http-client-api.md`
- component map: `docs/component-map.md`
- package/API manifests touched only if the build requires them

Do not add the helper to `conflux.net.http.client` unless the component graph
already links `http_async_client` for that import. A first-contact re-export can
be a later compatibility improvement, but the first patch should avoid silently
pulling io_uring async dependencies into the blocking client component.

### Phase 1 Tests

Add or update tests so users do not need to infer this behavior from internals:

- Local HTTP server smoke: `async_blocking_send(client, ClientRequest::get(...))`
  succeeds without mentioning `SocketTaskRing` in the test body.
- Lvalue and rvalue `ClientRequest` calls compile and run.
- Ring init failure returns `ClientResult` with `os_errno = -rc` and a message
  naming `io_uring_queue_init`. If no fixture/hook exists for forcing this,
  add one rather than leaving the path manually audited.
- Pump/runtime exception paths are converted to documented `ClientResult`
  errors, or the implementation documents and proves the path is unreachable.
- Keep at least one existing `SocketTaskRing` direct test so low-level async
  integration remains covered.
- Add public import smoke for `conflux.net.async_client` that calls
  `async_blocking_send` without importing private socket/io_uring modules.

## Phase 2: Persistent Daemon Reactor

Only start this after Phase 1 lands. The persistent reactor is useful, but it is
not just a convenience wrapper; it is a cross-thread runtime with cancellation,
shutdown, and queue semantics.

Proposed shape, subject to state-machine review:

```cpp
namespace conflux::http {

struct HttpClientReactorOptions {
  unsigned ring_entries = 256;
  std::size_t max_queue_depth = 1024;
};

class HttpClientReactor {
public:
  explicit HttpClientReactor(HttpClientOptions client_opts = {}, HttpClientReactorOptions reactor_opts = {});
  ~HttpClientReactor();

  HttpClientReactor(HttpClientReactor const&) = delete;
  HttpClientReactor& operator=(HttpClientReactor const&) = delete;
  HttpClientReactor(HttpClientReactor&&) = delete;
  HttpClientReactor& operator=(HttpClientReactor&&) = delete;

  [[nodiscard]] ClientResult send(ClientRequest request);
  void request_stop() noexcept;
};

} // namespace conflux::http
```

Do not expose `Task<ClientResult> async_send(...)` from the reactor in the first
persistent version. Returning a Conflux task across a foreign reactor thread
needs a precise task-source and cancellation bridge; `send(ClientRequest)` is
sufficient for daemon worker threads and avoids pretending the caller's task
runtime can directly drive the reactor-owned socket operations.

### Phase 2 Required State Machine

The implementation plan for Phase 2 must define these states before coding:

- `starting`: ring thread is being created; submissions fail with a normal
  `ClientResult` error until ready, or constructor blocks until ready.
- `running`: submissions are accepted up to `max_queue_depth`.
- `stopping`: no new submissions; queued but not started requests complete with
  cancellation-style `ClientResult` errors.
- `draining`: in-flight requests either complete naturally or are cancelled on
  the ring owner.
- `stopped`: thread joined; late submissions fail immediately.

Each queued request must own:

- a `ClientRequest`
- a completion state for `ClientResult`
- any cancellation flag/handle needed by the running socket task

The reactor must own:

- a reactor-thread `HttpClient`, constructed from stored `HttpClientOptions`
- `::io_uring`
- `CompletionTable`
- `SocketTaskRing`
- a wake fd or pipe
- a mutex-protected submission queue

The reactor must not store caller `HttpClient` or `ClientRequest` references
after `send(...)` returns from submission.

### Phase 2 Ring-Owner Contract

`SocketTaskRingOptions::submit_on_ring_owner` must enqueue a `RingOpFn` onto the
reactor queue and wake the reactor. It must:

- return `false` after `stopping` begins
- never invoke the callback inline from the caller thread
- preserve FIFO ordering for callbacks submitted by the same caller thread
- tolerate wake fd failure by moving the reactor to `stopping` and completing
  affected requests with errors
- drain late CQEs during shutdown before destroying `CompletionTable`

### Phase 2 Cancellation And Timeout

Do not add a reactor-specific timeout option in Phase 2 unless cancellation is
implemented all the way down to the socket operation. The first persistent
reactor should rely on `ClientRequest::timeouts()` for HTTP phase deadlines.

If explicit external cancellation is added later, it must prove:

- cancellation is routed via `submit_on_ring_owner`
- cancellation during DNS/connect/write/read completes the waiting caller
- the reactor remains usable for a later request
- destructor handles queued and in-flight cancellation without hanging

### Phase 2 Tests

Required before shipping the persistent reactor:

- multiple sequential requests on one reactor
- concurrent submissions from several caller threads
- queue-full behavior
- destruction with queued work
- destruction with in-flight connect/read work
- request-level timeout during connect or first-byte wait, followed by a
  successful later request on the same reactor
- `submit_on_ring_owner` is not invoked inline from the caller thread
- public import smoke for the chosen module/component

## Documentation Updates

Update the HTTP client docs to present the decision tree:

- `HttpClient::blocking_send`: simplest synchronous path, no io_uring runtime.
- `async_blocking_send`: one-shot helper that uses the async transport without
  exposing ring setup.
- `async_send(..., SocketTaskRing&, ...)`: advanced integration with caller-owned
  socket reactor.
- `HttpClientReactor` once Phase 2 exists: persistent outbound HTTP runtime for
  daemons.

## Non-Goals

- Replacing `SocketTaskRing`.
- Changing HTTP server internals.
- Changing the `ClientResult` error model.
- Solving the Postgres persistent reactor problem in this patch.
- Introducing a new HTTP parser/client implementation.
- Shipping a generic cross-component io_uring runtime before the HTTP use case
  is proven.

## Acceptance Criteria

Phase 1 is complete when:

- outbound async-backed HTTP can be performed without naming `SocketTaskRing`,
  `SocketRawRing`, `CompletionTable`, or `io_uring`
- return type and errors match existing `ClientResult` contracts
- common lvalue/rvalue request calls compile without casts
- docs explain when to choose blocking, async-backed blocking, or low-level async
- focused tests and public import smoke pass

Phase 2 is not ready to implement until its state machine and ring-owner
contract are reviewed separately and score at least 9/10.
