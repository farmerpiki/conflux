# Pre-v1 Migration Contract

This is the current project contract while Conflux HTTP/work APIs are still
pre-v1.

For release-level versioning, security disclosure, compiler, and kernel/runtime policy, see [`project-policy.md`](project-policy.md).
For task/executor placement and HTTP ring-thread handler rules, see [`execution-model.md`](execution-model.md).
For code-review rules around handler placement and `blocking_`/`sync_`/`async_` naming, see
[`concurrency-naming-model.md`](concurrency-naming-model.md).

- `liburing` is required for runtime, HTTP, and socket targets. After the modular target split lands, `conflux::core`, `conflux::json`, `conflux::file_io_sync`, and `conflux::file_map` will not require liburing. Conflux remains Linux/io_uring-first.
- Breaking API changes are expected before v1 when they simplify the final
  public surface.
- `conflux.work` legacy `Flow<T>` APIs are deprecated for new code and are
  scheduled for removal after transitional users migrate.
- `conflux.work.root` is the future async base for public-facing async flows.
- All task progress is executor-owned. There is no supported task model that
  runs outside an executor; the current execution backends are the work/uring
  combination (`WorkPool` and ring-thread executors).
- HTTP server handlers run on io_uring ring threads. The easy layer may hide
  setup details, but it does not silently move arbitrary synchronous handlers to
  a worker pool.
- Advanced users can still explicitly choose execution placement by returning
  task-based async work, scheduling work on a chosen executor, or calling
  raw syscall-style helpers whose `blocking_*` names make thread-blocking cost
  explicit.
- `http::App` is the preferred first-contact surface and includes core routing
  ergonomics (`get/post/put/patch/del/options`, `use`, `sse`, `group`,
  `on_not_found`, `on_error`), while still exposing `config()` and `router()`
  for advanced tuning.

Current HTTP direction:

- Keep synchronous handlers supported, but they run on ring threads and must stay
  short, bounded, and non-blocking.
- Prefer root-backed async handlers (`root::Task<HttpResponse>`) when the work
  has explicit suspension points.
- Use explicit executor handoff for executor-owned chains. Reserve
  `blocking_*` names for raw syscall-style helpers that can block the calling
  thread; do not rely on hidden handler auto-offload.
- Keep low-level controls available for expert use, without making them the
  first-contact API.

## Example Audience Split

Default easy HTTP examples:

- `examples/hello.cxx`
- `examples/middleware.cxx`
- `examples/sse.cxx`
- `examples/static.cxx`

Advanced runtime/feature examples:

- `examples/file_io.cxx`
- `examples/coroutines.cxx` (legacy `Flow<T>` coroutine style)
- `examples/dual.cxx`
- `examples/h3_server.cxx`
- `examples/h3_probe.cxx`
- `examples/db_basic.cxx`
- `examples/db_pool.cxx`

## Default vs Advanced Handler Shapes

Use the simplest shape that matches the work, while preserving the placement
contract from `docs/execution-model.md` and the review checklist in
`docs/concurrency-naming-model.md`:

- Fast synchronous work: accept `HttpRequestView` and return
  `HttpResponse` directly. `HttpRequestView` is the first-contact sync request
  type. This code executes on the HTTP ring thread.
- Async workflow with coroutine-style composition: accept owning `HttpRequest`
  and return `conflux::work::root::Task<HttpResponse>`. Task progress is
  executor-owned; borrowed request views must not cross suspension.
- Blocking or heavy CPU work must be made explicit: schedule executor-owned
  work through the chosen executor, or call a raw syscall-style helper whose
  `blocking_*` name advertises calling-thread blocking behavior.

Example:

```cpp
import conflux.net.http.server;
import conflux.work;

namespace http = conflux::http;

int main() {
	auto app = http::App::default_server();

	app.get("/sync", [](HttpRequestView const &) {
		return HttpResponse::text("ok");
	});

	app.get("/task", [](HttpRequest const &) -> conflux::work::root::Task<HttpResponse> {
		auto [task, source] = conflux::work::root::make_task_source<HttpResponse>();
		(void)source.commit_success(conflux::work::root::Success<HttpResponse>{HttpResponse::text("task-ok")});
		return std::move(task);
	});

	auto pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 2});
	app.get("/defer", [pool](HttpRequestView const &) {
		return http::defer(pool, [] { return HttpResponse::text("defer-ok"); });
	});

	std::move(app).run({.port = 9090});
}
```

## Blocking Handler Guardrails

Synchronous route handlers run on the HTTP server ring thread. They are intended
for short, non-blocking work such as routing decisions, header/body inspection,
small in-memory transformations, and immediate response construction. CPU-heavy
work, disk I/O, DNS, blocking HTTP clients, database calls, and other operations
that can stall must not be hidden inside ordinary sync handlers.

Preferred explicit options:

- Return `conflux::work::root::Task<HttpResponse>` when the handler naturally
  composes with coroutine/task suspension. The task still progresses through an
  executor; there is no non-executor task path.
- Use a caller-owned executor/work pool for blocking callables where that is the
  intended placement.
- Prefer `blocking_*` only for raw syscall-style helpers that may block the
  calling thread, so ring-thread-unsuitable calls are visible at review time.
  Executor-owned non-coroutine chains should use `sync_*`; coroutine APIs should
  use `async_*`.

Synchronous handlers are supported on-ring, and ring-thread blocking can be
surfaced with opt-in diagnostics:

- `Config::slow_handler_diagnostics` (default `false`)
- `Config::slow_handler_warn_ms` (default `25`)

When enabled, requests whose synchronous handler execution time crosses the
threshold emit a warning to `stderr` with method, path, and elapsed ms.

Example:

```cpp
import conflux.net.http.server;

namespace http = conflux::http;

int main() {
	auto app = http::App::default_server();
	app.config().slow_handler_diagnostics = true;
	app.config().slow_handler_warn_ms = 10;
	app.get("/ping", [](HttpRequest const &) { return HttpResponse::text("ok"); });
	std::move(app).run({.port = 9090});
}
```
