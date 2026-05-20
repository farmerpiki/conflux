# Pre-v1 Migration Contract

This is the current project contract while Conflux HTTP/work APIs are still
pre-v1.

For release-level versioning, security disclosure, compiler, and kernel/runtime policy, see [`project-policy.md`](project-policy.md).
For task/executor placement and HTTP ring-thread handler rules, see [`execution-model.md`](execution-model.md).
For code-review rules around handler placement and `blocking_`/`sync_`/`async_` naming, see
[`concurrency-naming-model.md`](concurrency-naming-model.md).

- `liburing` is required for runtime-facing components, including HTTP
  server/client transport components, async file I/O, socket I/O, and tests or
  benchmarks that exercise those surfaces. `conflux::core`, `conflux::json`,
  `conflux::file_io_sync`, and `conflux::file_map` do not require liburing.
  Conflux remains Linux/io_uring-first.
- Breaking API changes are expected before v1 when they simplify the final
  public surface.
- A configured package exposes one public consumer interface. Use
  `MODULE_INTERFACE` with `import conflux.*` or `HEADER_INTERFACE` with generated
  headers; mixed import/include consumption is unsupported.
- `conflux.work` legacy `Flow<T>` APIs have been removed from the current source;
  migration docs retain historical spellings only as before-state examples.
- `conflux.work.root` is the async base for public-facing async flows.
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
  ergonomics (`get/post/put/patch/del/options`, context-aware handlers, `use`,
  `sse`, `ws`, `serve_static`, `group`, `on_not_found`, `on_error`), while
  still exposing `config()` and `router()` for advanced tuning.

Current HTTP direction:

- Keep synchronous handlers supported, but they run on ring threads and must stay
  short, bounded, and non-blocking.
- Prefer facade async handlers (`http::Task<http::Response>`) when the work
  has explicit suspension points.
- Use explicit executor handoff for executor-owned chains. Reserve
  `blocking_*` names for raw syscall-style helpers that can block the calling
  thread; do not rely on hidden handler auto-offload.
- Keep low-level controls available for expert use, without making them the
  first-contact API.

## Example Audience Split

Default easy HTTP examples:

- `examples/quickstart/hello.cxx`
- `examples/quickstart/json_crud.cxx`
- `examples/quickstart/middleware.cxx`
- `examples/quickstart/static_files.cxx`
- `examples/quickstart/sse.cxx`
- `examples/quickstart/websocket.cxx`
- `examples/quickstart/openapi.cxx`
- `examples/quickstart/postgres_json.cxx`

Top-level HTTP examples remain public-facing, but `examples/quickstart/` is the
first-contact path for new users.

Advanced runtime/feature examples:

- `examples/advanced/file_io.cxx`
- `examples/advanced/coroutines.cxx` (`root::Task<T>` coroutine file-I/O style)
- `examples/advanced/dual.cxx`
- `examples/advanced/h3_server.cxx`
- `examples/advanced/h3_probe.cxx`
- `examples/advanced/db_basic.cxx`
- `examples/advanced/db_pool.cxx`
- `examples/advanced/postgres.cxx`

## Default vs Advanced Handler Shapes

Use the simplest shape that matches the work, while preserving the placement
contract from `docs/execution-model.md` and the review checklist in
`docs/concurrency-naming-model.md`:

- Fast synchronous work: accept `http::RequestView` and return
  `http::Response` directly. `http::RequestView` is the first-contact sync request
  type. This code executes on the HTTP ring thread.
- Async workflow with coroutine-style composition: accept owning `http::Request`
  and return `http::Task<http::Response>`. Task progress is
  executor-owned; borrowed request views must not cross suspension.
- Blocking or heavy CPU work must be made explicit: schedule executor-owned
  work through the chosen executor, or call a raw syscall-style helper whose
  `blocking_*` name advertises calling-thread blocking behavior.

Example:

```cpp
import conflux.http;
import std;

namespace http = conflux::http;

int main() {
	auto app = http::app();

	app.get("/sync", [](http::RequestView const &) {
		return http::text("ok");
	});

	app.get("/task", [](http::Request const &) -> http::Task<http::Response> {
		co_return http::text("task-ok");
	});

	app.get("/context", [](http::Request const &, http::RequestContext const &) -> http::Task<http::Response> {
		co_return http::text("context-ok");
	});

	return http::run(std::move(app), {.port = 9090});
}
```

## Blocking Handler Guardrails

Synchronous route handlers run on the HTTP server ring thread. They are intended
for short, non-blocking work such as routing decisions, header/body inspection,
small in-memory transformations, and immediate response construction. CPU-heavy
work, disk I/O, DNS, blocking HTTP clients, database calls, and other operations
that can stall must not be hidden inside ordinary sync handlers.

Preferred explicit options:

- Return `http::Task<http::Response>` when the handler naturally
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
import conflux.http;

namespace http = conflux::http;

int main() {
	auto app = http::app();
	app.config().slow_handler_diagnostics = true;
	app.config().slow_handler_warn_ms = 10;
	app.get("/ping", [](http::Request const &) { return http::text("ok"); });
	return http::run(std::move(app), {.port = 9090});
}
```
