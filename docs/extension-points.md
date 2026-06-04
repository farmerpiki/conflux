# Extension Point Registry

This registry names the preview customization points that are intended for
application or integration code. It does not promise a generic plugin system;
unsupported categories stay explicit so applications do not depend on hidden
hooks.

| Extension point | Preview status | Public API | Minimal example | Evidence |
|---|---|---|---|---|
| JSON codec | Stable preview | `conflux::json::JsonMembers<T>`, `conflux::json::JsonCodec<T>`, `http::codec::json::try_response_with<Provider>` | `examples/advanced/manual_json_members.cxx`, `examples/advanced/custom_json_provider.cxx` | `tests/json_codec_members_test.cxx`, `tests/HttpFacadeTests.cmake` |
| Body encoder/decoder | Stable preview for JSON route bodies | `http::json(value)`, `http::json_body<T>`, `http::codec::json::routes<Provider>(app)` | `examples/quickstart/json_crud.cxx`, `examples/advanced/custom_json_provider.cxx` | `tests/http_facade_response_test.cxx`, `tests/http_facade_extractors_test.cxx` |
| Auth validator | Stable preview | `basic_auth_middleware(validator)`, `bearer_auth_middleware(validator)`, `auth_throttle_middleware(...)` | `examples/public/middleware.cxx`, `examples/advanced/http_policy_stack.cxx` | `tests/HttpAuthTests.cmake`, `tests/http_auth_rate_limit_e2e.cxx` |
| DB pool | Stable preview, application-owned | `conflux::pg::Pool`, `PoolConfig`, `Pool::acquire()` | `examples/advanced/db_basic.cxx`, `examples/advanced/db_pool.cxx` | `tests/DbTests.cmake`, `tests/db_integration_test.cxx` |
| Logging sink | Stable preview | `make_access_log_middleware(sink)`, `structured_log_middleware(options)` | `examples/public/middleware.cxx`, `examples/advanced/http_policy_stack.cxx` | `tests/HttpObservabilityTests.cmake`, `tests/http_structured_log_e2e.cxx` |
| Metrics sink | Stable preview | `http::observability(options, sinks)`, `ObservabilitySinks`, `observability_server_hooks()` | `examples/advanced/http_observability.cxx`, `examples/advanced/production_showcase.cxx` | `tests/http_facade_observability_test.cxx`, `scripts/check-observability-docs.py` |
| Tracing propagation | Stable preview | `tracing_middleware(TracingOptions)`, `http::observability(...)` trace options | `examples/public/middleware.cxx`, `examples/advanced/http_policy_stack.cxx` | `tests/http_tracing_e2e.cxx`, `tests/HttpObservabilityTests.cmake` |
| TLS provider | Fixed provider in preview | OpenSSL-backed `Config` TLS fields and capability diagnostics | `examples/advanced/production_showcase.cxx` | `docs/configuration.md`, `tests/http_tls_e2e.cxx` |
| Static-file cache policy | Stable preview | `StaticFileCacheConfig`, `StaticOptions::file_cache`, `Router::set_static_file_cache(...)` | `examples/quickstart/static_files.cxx`, `examples/public/static.cxx` | `tests/http_static_*`, `scripts/check-security-posture-docs.py` |
| Allocator/arena | Stable preview for JSON arena ownership | `JsonArena::parse_into(...)`, `JsonParsePolicy`, allocation diagnostics | `examples/advanced/json.cxx`, `examples/advanced/json_stream_ingest.cxx` | `tests/json_storage_test.cxx`, `scripts/check-cost-lifetime-docs.py` |
| Runtime/executor integration | Stable preview | `WorkPool`, `WorkPoolOptions`, `RingLane`, `async_run_on(...)` | `examples/advanced/explicit_offload.cxx`, `examples/advanced/work_join_all.cxx` | `tests/work_test.cxx`, `tests/api_surface_extended_import_smoke.cxx` |

Extension-point pages:

- JSON codec and provider boundaries: `docs/json-api.md`, `docs/json-boundary-guide.md`.
- Body encoder/decoder helpers: `docs/http-server-api.md` JSON route responses.
- Auth validators and rate limits: `docs/auth-rate-limit-hooks.md`, `docs/auth-password-hashing.md`.
- DB pool: `docs/db-api.md`.
- Logging, metrics, and tracing: `docs/observability.md`.
- TLS provider and deployment config: `docs/configuration.md`.
- Static-file cache policy: `docs/http-server-api.md` static file serving.
- Allocator/arena: `docs/cost-lifetime-model.md`, `docs/json-api.md`.
- Runtime/executor integration: `docs/conflux-work-root-api.md`, `docs/execution-model.md`.

Installed-package compile evidence is intentionally tied to public examples,
facade snapshot tests, and component smoke tests instead of a separate plugin
ABI. New extension points should add a row here, a minimal example, and a
compile or e2e guard that imports the advertised package component.
