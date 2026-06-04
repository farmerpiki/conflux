# Task Path

Use this page when choosing the first document or example for a user task. Each
row states the import/include surface, the cost or lifetime note to check, a
minimal example, and the advanced escape hatch.

| Task | Import/include | Cost/lifetime note | Minimal example | Advanced escape hatch |
|---|---|---|---|---|
| hello world | `import conflux;` / `conflux::http` | Ring-thread handler; keep work short and non-blocking. | `examples/quickstart/hello.cxx` | `docs/http-server-api.md` lower-level server ownership. |
| JSON API | `import conflux;` plus `conflux::http::json_body<T>` / `http::codec::json` | JSON request bodies are bounded and parsed explicitly; provider choice stays visible. | `examples/quickstart/json_crud.cxx` | `examples/advanced/custom_json_provider.cxx`, `docs/json-boundary-guide.md`. |
| typed path/query/body extraction | `import conflux;` / typed `app.get<"...">` and extractor parameters | Extractors validate before handler body; request views borrow active request storage. | `examples/quickstart/openapi.cxx` | `docs/http-server-api.md` route metadata and validation sections. |
| error handling | `import conflux;` / `http::problem`, `http::bad_request`, `app.on_error` | Error responses allocate normal response bodies; avoid leaking sensitive details. | `examples/quickstart/json_crud.cxx` | `docs/http-server-api.md` response helper and security sections. |
| middleware | `import conflux;` / `app.use(...)` | First-contact middleware is sync and ring-thread local; async context middleware is advanced. | `examples/quickstart/middleware.cxx` | `examples/public/middleware.cxx`, `conflux.http.extended`. |
| auth | `import conflux;` plus auth middleware modules when needed | Authentication callbacks must be bounded; password hashing and DB-backed auth need explicit offload. | `examples/public/middleware.cxx` | `docs/auth-password-hashing.md`, `docs/auth-rate-limit-hooks.md`. |
| DB access | `import conflux.pg;` / `conflux::pg` | DB operations are coroutine tasks; pool acquires wait up to `PoolConfig::acquire_timeout`. | `examples/advanced/db_basic.cxx` | `examples/advanced/db_pool.cxx`, `docs/db-api.md`. |
| static files | `import conflux;` / `app.serve_static(...)` | Static mounts enforce traversal policy; blocking file reads need explicit offload or cache policy. | `examples/quickstart/static_files.cxx` | `examples/public/static.cxx`, static options in `docs/http-server-api.md`. |
| uploads/multipart | `import conflux;` / `http::UploadedFile` | Uploaded file views borrow request-body storage; call `to_owned()` before escape. | `examples/public/forms.cxx` | `docs/http-server-api.md` request and upload sections. |
| SSE/WebSocket | `import conflux;` / `app.sse(...)`, `app.ws(...)` | SSE queues have explicit overflow policy; WebSocket send calls block the owning worker. | `examples/quickstart/sse.cxx`, `examples/quickstart/websocket.cxx` | `docs/http-server-api.md` SSE and WebSocket sections. |
| observability | `import conflux;` / `http::observability(...)` | Keep labels low-cardinality; pressure/work-pool/task metrics are explicit sources. | `examples/advanced/http_observability.cxx` | `docs/observability.md`, `examples/advanced/production_showcase.cxx`. |
| graceful shutdown | `import conflux;` / `HttpServer::drain`, `shutdown`, `port` | `port()` waits for listen readiness; `drain(options)` owns stop/close/finish policy. | `examples/advanced/http_lifecycle.cxx` | `docs/production-checklist.md` lifecycle section. |
| deployment config | `import conflux;` / `Config::public_server`, INI config helpers | Presets are bounded by default; diagnostics should use redacted summaries. | `examples/advanced/production_showcase.cxx` | `docs/configuration.md`, `docs/production-checklist.md`. |

Reference pages remain authoritative for full signatures and release gates:
`docs/http-server-api.md`, `docs/json-api.md`, `docs/db-api.md`,
`docs/observability.md`, `docs/configuration.md`, and
`docs/cost-lifetime-model.md`.
