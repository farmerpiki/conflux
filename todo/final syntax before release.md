> Status: superseded by `docs/execution-model.md` and `docs/naming-audit.md`.
> This file is historical planning material. Do not use it to justify broad
> high-level `blocking_*` names; current policy reserves `blocking_*` for raw
> blocking syscall-style helpers unless an explicit later design decision widens
> that family.

## Public execution API families

Conflux uses execution prefixes to describe both dependency shape and caller-thread behavior.

### `blocking_*`

`blocking_*` APIs are runtime-free caller-thread blocking APIs. They do not require
`io_uring`, `SocketTaskRing`, `WorkPool`, or Conflux runtime ownership. They may
wrap low-level syscalls or provide high-level convenience such as reading a whole
file, sending an HTTP request through blocking socket/poll code, or loading a
template from disk.

`blocking_*` APIs are intended for CLI tools, setup code, tests, and users who
only want standalone components such as JSON, file helpers, or templating. Their
syntax should stay close to the matching `sync_*` and `async_*` APIs so callers
can migrate without reshaping request/options/result code.

Pure CPU/value APIs do not need an execution prefix. `json::parse(string_view)`
and `template::render(...)` are ordinary APIs. File-backed convenience wrappers
around them may be `blocking_*`.

### `sync_*`

`sync_*` APIs are synchronous caller-surface APIs backed by Conflux runtime,
executor, or `io_uring` machinery. They may block the calling thread while waiting
for completion, but the default implementation must not consume an HTTP/server
ring thread as the waiting/progress thread.

The default `sync_*` mode submits work to a Conflux-owned or caller-provided
runtime and waits for the result from the calling thread. It is for ordinary
blocking application threads that want high-performance Conflux I/O without
writing coroutines.

Calling `sync_*` from a Conflux ring thread is a placement error unless an
explicit expert/current-thread option is used and documented at the call site.

A current-thread driving mode may exist for CLI, tests, embedding, and expert
single-thread use, but it is not the default `sync_*` behavior.

### `async_*`

`async_*` APIs are coroutine/task APIs. They return `root::Task<T>` or another
explicit awaitable and progress only through executor-owned machinery. They do
not block the caller thread.

`async_*` APIs are intended for native Conflux async code and HTTP handlers that
already execute on a ring/executor. They should remain light: async I/O and small
glue belong directly in the coroutine; blocking calls and CPU-heavy work require
explicit executor/work-pool placement.
