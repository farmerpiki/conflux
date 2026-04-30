# Pre-v1 Migration Contract

This is the current project contract while Conflux HTTP/work APIs are still
pre-v1.

- `liburing` is required. Conflux remains Linux/io_uring-first.
- Breaking API changes are expected before v1 when they simplify the final
  public surface.
- `conflux.work` legacy `Flow<T>` APIs are deprecated for new code and are
  scheduled for removal after transitional users migrate.
- `conflux.work.root` is the future async base for public-facing async flows.
- The HTTP easy layer (`conflux::http::App`) hides runtime placement details by
  default.
- Advanced users can still explicitly choose execution placement (ring-affine
  work, work-pool work, and root-task-driven async handlers).
- `http::App` is the preferred first-contact surface and includes core routing
  ergonomics (`get/post/put/patch/del/options`, `use`, `sse`, `group`,
  `on_not_found`, `on_error`), while still exposing `config()` and `router()`
  for advanced tuning.

Current HTTP direction:

- Keep synchronous handlers supported.
- Prefer root-backed async handlers (`root::Task<HttpResponse>`) and helper
  deferral APIs for long-running work.
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

Use the simplest shape that matches the work:

- Fast synchronous work: return `HttpResponse` directly.
- Async workflow with coroutine-style composition: return
  `conflux::work::root::Task<HttpResponse>`.
- Offload blocking or heavy CPU work to a pool without exposing
  `DeferredResponse`: use `conflux::http::defer(...)`.

Example:

```cpp
import conflux.net.http;
import conflux.work;

namespace http = conflux::http;

int main() {
	auto app = http::App::default_server();

	app.get("/sync", [](http::Request const &) {
		return http::Response::text("ok");
	});

	app.get("/task", [](http::Request const &) -> conflux::work::root::Task<http::Response> {
		auto [task, source] = conflux::work::root::make_task_source<http::Response>();
		(void)source.commit_success(conflux::work::root::Success<http::Response>{http::Response::text("task-ok")});
		return std::move(task);
	});

	auto pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 2});
	app.get("/defer", [pool](http::Request const &) {
		return http::defer(pool, [] { return http::Response::text("defer-ok"); });
	});

	std::move(app).run({.port = 9090});
}
```

## Blocking Handler Guardrails

Synchronous handlers are still supported, but ring-thread blocking can now be
surfaced with opt-in diagnostics:

- `Config::slow_handler_diagnostics` (default `false`)
- `Config::slow_handler_warn_ms` (default `25`)

When enabled, requests whose synchronous handler execution time crosses the
threshold emit a warning to `stderr` with method, path, and elapsed ms.

Example:

```cpp
import conflux.net.http;

namespace http = conflux::http;

int main() {
	auto app = http::App::default_server();
	app.config().slow_handler_diagnostics = true;
	app.config().slow_handler_warn_ms = 10;
	app.get("/ping", [](http::Request const &) { return http::Response::text("ok"); });
	std::move(app).run({.port = 9090});
}
```
