# Concurrency and Naming Model

This is the canonical pre-v1 review guide for task placement, HTTP handler
execution, and execution-name prefixes. Use it when reviewing worker, HTTP,
file/network I/O, DB, process, and app-route changes.

## One task model

All `conflux` tasks progress through an executor. There is no supported task path
that runs outside executor ownership, falls back to an arbitrary caller thread,
or relies on hidden background progress.

Current executor families:

- `WorkPool` for ordinary worker-pool execution;
- `RingLane` / `SocketTaskRing` for io_uring-coupled ring-thread execution.

New task APIs may add adapters or new executor backends, but they must keep task
progress executor-owned and must not introduce a second non-executor task model.

## HTTP handler placement

HTTP server handlers run on the HTTP server's io_uring ring threads. This is the
placement contract.

Do not add a compatibility layer that accepts arbitrary synchronous HTTP handlers
and secretly moves them to a worker pool. Hidden offload would make latency
placement ambiguous, split the handler mental model, and make request lifetime
ownership harder to reason about.

Supported handler shapes:

| Shape | Placement | Intended use |
|---|---|---|
| `http::Response` from `http::RequestView` or `http::Request` | ring thread | short, bounded, non-blocking work |
| `conflux::work::Task<http::Response>` from normalized `http::RequestView` or owning `http::Request` | executor-owned task progress | workflows with explicit suspension points |
| deferred/streaming response | ring-owned response handle plus explicit async writer task | chunked output, SSE-style response bodies, delayed completion |
| explicit `http::offload(pool, ...)` or equivalent caller-owned executor handoff | chosen worker/executor | blocking or CPU-heavy work made visible at the call site |

`http::RequestView` is borrowed from the active request buffer. It may suspend
only through normalized server/app async dispatch paths that pin request storage
for the deferred task lifetime and keep the view object in the coroutine chain.
Raw caller-owned `RequestView` tasks must keep backing storage alive externally
or use `http::OwnedRequest` / copied fields for escaped request data.

Synchronous ring-thread handlers may parse headers, inspect already-buffered body
data, build responses, update small in-memory state, enqueue explicit async work,
or return a deferred response. They must not perform unbounded disk I/O, blocking
DNS, blocking HTTP client calls, blocking DB calls, sleeps, or heavy CPU work
inline on the ring thread.

Slow-handler diagnostics are guardrails only. They can reveal ring-thread stalls;
they do not change where handlers execute.

## Prefix policy

The long-term public naming model is prefix-based:

| Prefix | Meaning | Examples |
|---|---|---|
| `blocking_*` | raw blocking syscall-style helper that may block the calling thread | direct `open/read/write/fsync/stat/mmap` wrappers, direct blocking socket/process helpers |
| `sync_*` | executor/task API with a non-coroutine caller surface; the chain still runs through the task/executor model | synchronous facade over executor-owned work, explicit wait-style compatibility adapter |
| `async_*` | coroutine/task API with explicit suspension | `root::Task<T>`-returning operations, socket/file/network coroutines |

`blocking_*` is intentionally narrow. Do not use it for every non-coroutine API.
If the implementation is executor-owned but the public surface waits or reports a
plain result, use `sync_*`. If the API returns a coroutine/task or is intended to
be `co_await`ed, use `async_*`.

Current code may still contain suffix-style names such as `*_sync`, `*_async`,
and ordinary names that predate this policy. Public preview surfaces should use
the final documented names and should not advertise deprecated compatibility
aliases. Add clearer names only in component-local branches and update local
call sites without broad churn.

## Component-boundary rules

- Low-level POSIX helpers that can block the calling thread should be named
  `blocking_*` and should not live in targets that force all users to link the
  io_uring runtime.
- Executor-owned non-coroutine surfaces should be named `sync_*`, not
  `blocking_*`.
- Coroutine APIs should be named `async_*` and return explicit task/coroutine
  vocabulary.
- Pure value APIs do not need execution prefixes. Examples: JSON parse/write over
  buffers, HTTP response builders, header/cookie helpers, URL parsing, route
  registration, and metadata accessors.
- JSON remains buffer/view/provider-boundary based. It must not gain hidden file
  I/O; file convenience adapters belong in separate file-oriented modules.

## Review checklist

Reject or request redesign when a patch:

- adds a task type or task progress path that is not executor-owned;
- hides arbitrary synchronous HTTP handler work behind automatic worker-pool
  offload;
- makes an `http::RequestView` coroutine handler possible across suspension
  without a dispatch-owned request-storage lease;
- calls blocking disk, DNS, HTTP client, DB, sleep, or heavy CPU work inline from
  a ring-thread synchronous handler;
- names an executor-owned synchronous facade `blocking_*`;
- names a raw caller-thread-blocking syscall helper `sync_*`;
- adds a coroutine/task API without an `async_*` final-shape name or a documented
  exception;
- removes unrelated names outside the component being cleaned.

Prefer patches that:

- keep synchronous HTTP handlers short and ring-local;
- make worker-pool or executor handoff explicit at the handler call site;
- use normalized dispatch-owned request leases for async view handlers, or
  owning `http::Request` when storage is caller-owned or must escape dispatch;
- isolate temporary wait bridges behind one clearly named fallback adapter;
- update `docs/naming-audit.md` instead of renaming unrelated surfaces.

## Anti-goals

Do not implement:

- hidden auto-offload for arbitrary HTTP handlers;
- a task model that progresses without an executor;
- a second "simple sync" server runtime outside the ring/executor model;
- broad `blocking_*` naming for executor-owned sync chains;
- release-wide unrelated renaming in feature branches.
