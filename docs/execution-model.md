# Execution Model Contract

This document records the current pre-v1 execution contract for work, HTTP, and
API naming. It is intentionally explicit because the public names will still go
through a larger cleanup before v1.

## Core rule

All `conflux` tasks execute on an executor. There is no supported task model
where a task progresses outside executor ownership or falls back to an ad-hoc
caller-thread path.

The current executor backends are:

- `WorkPool` — thread-pool executor for ordinary worker execution;
- `RingLane` / `SocketTaskRing` — io_uring-coupled ring-thread execution.

Future executor work may add backends or adapters, but must preserve the rule
that task progress is executor-owned.

## HTTP server placement

HTTP server handlers run on the HTTP server's io_uring ring threads. This is a
contract, not a bug to hide behind automatic offload.

Do not add a compatibility layer that takes arbitrary synchronous HTTP handlers
and silently dispatches them to a work pool. That would create two execution
models, obscure latency placement, and complicate request lifetime ownership.

Ring-thread handlers may parse headers, inspect already-buffered request data,
construct responses, update small in-memory state, enqueue explicit async work,
or return a task/deferred response. They must not perform unbounded disk I/O,
blocking DNS, blocking client HTTP, blocking DB calls, sleeps, or heavy CPU work
inline on the ring thread.

Slow-handler diagnostics are useful as guardrails, but they do not change where
handlers run.

## Public naming model

The long-term public naming model is prefix-based:

- `blocking_*` means a raw blocking syscall-style helper. Examples include thin
  wrappers around calls shaped like `::write(fd, ...)`, direct `open/read/write`
  file helpers, and process/syscall helpers that can block the calling thread.
- `sync_*` means a synchronous API shape that runs through the task/executor
  model and reports success/failure of an execution chain without coroutine
  syntax.
- `async_*` means a coroutine/task-based API shape with explicit suspension.

`blocking_*` is not a synonym for every non-coroutine function. It is reserved
for direct blocking operations at system-call-style boundaries. `sync_*` is for
executor-owned chains that present a synchronous/non-coroutine user surface.
`async_*` is for coroutine APIs.

Current code still contains older names such as `*_sync` or `*_async`. Those are
pre-v1 transition names. `docs/naming-audit.md` records the known inventory and
rename order for the later heavy renaming pass that should align public APIs to
`blocking_*`, `sync_*`, and `async_*` consistently.

## Component-boundary implications

The naming split is also a dependency-boundary rule:

- low-level POSIX helpers that do not require `io_uring` belong outside the
  runtime/liburing targets; direct blocking syscall-style helpers should move
  toward `blocking_*` names;
- executor/task APIs should not be described as `blocking_*` merely because their
  public surface is non-coroutine; use `sync_*` for those chains;
- coroutine APIs should use `async_*` names and explicit task/coroutine return
  types;
- `conflux::json` remains buffer/view based and should not gain hidden file I/O;
- optional file convenience adapters such as `conflux::json_file` may use the
  POSIX helper target without making all JSON users link the runtime.

## Anti-goals

Do not implement:

- hidden auto-offload for arbitrary HTTP handlers;
- a task type that progresses without an executor;
- a second "simple sync" server runtime outside the ring/executor model;
- broad `blocking_*` naming for executor-owned sync chains.
