# Component, package, and documentation map

This map gives downstream users and future implementation branches one place to
answer three questions:

1. which feature bundle to configure;
2. which CMake package component/target to link;
3. which API document or example owns the public contract.

The CMake source remains authoritative. Keep this file synchronized with
`cmake/ConfluxPresets.cmake` and the `conflux_public_component(...)` calls in
`CMakeLists.txt` whenever components are split, renamed, or removed. API-surface
classification here is coarse; detailed profile contents live in
[`api-surface-profiles.md`](api-surface-profiles.md).

## Feature bundles

`CONFLUX_FEATURE_SET` selects defaults for the `CONFLUX_BUILD_*` component flags.
Explicit `CONFLUX_BUILD_*=ON/OFF` cache values override the bundle defaults.
`CONFLUX_API_SURFACE=curated|extended|complete` is separate: it controls only the
aggregate import/include surface selected by `import conflux;` and
`<conflux.hxx>`.

| Bundle | Intended use | Notable defaults |
|---|---|---|
| `core` | liburing-free vocabulary-only consumer | core/types only |
| `work` | io_uring/work/socket runtime experiments | work + socket I/O |
| `json` | JSON parser/DOM/SAX/NDJSON only | JSON without HTTP/runtime |
| `http-minimal` | smallest HTTP server app shape | runtime, file I/O, DNS, crypto, JSON, HTTP core/router/server/JSON |
| `http-api` | HTTP API service with policy/auth | `http-minimal` + policy + auth |
| `http-api-full` | API service with observability/spec output | `http-api` + observability + OpenAPI |
| `web-server` | static/template/realtime web serving | HTTP server, static files, compression, realtime, templates |
| `http-server-complete` | full HTTP stack without DB/SMTP/process/file-watch extras | HTTP server/client/proxy/vhost/policy/auth/observability/OpenAPI |
| `complete` | aggregate validation and local experimentation | HTTP, runtime, JSON, DB, SMTP, process, file-watch; JSON file/reflection remain opt-in |

For first-contact public use, prefer `core`, `json`, `http-minimal`, `http-api`,
and `pg` when PostgreSQL support is included in the release evidence. Use
`complete` only when intentionally testing aggregate development surface area.

## Downstream CMake usage

Choose one public interface mode when configuring the package:

```cmake
-DCONFLUX_INTERFACE_MODE=MODULE_INTERFACE
-DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE
```

`MODULE_INTERFACE` consumers import `conflux.*` modules and are the primary
source-consumption path for the preview. `HEADER_INTERFACE` consumers include
generated headers from staged release artifacts. Mixing module imports and
generated headers in one consumer package or executable is unsupported.

Install a build, then consume the exported package by component:

```cmake
find_package(conflux REQUIRED COMPONENTS core json http_server)

target_link_libraries(my_app PRIVATE
    conflux::core
    conflux::json
    conflux::http_server)
```

`conflux::conflux` is the public umbrella target when the aggregate was built and
installed. Prefer narrow component targets for new examples and docs so optional
protocol/storage dependencies stay visible.

Validate an install tree with:

```sh
scripts/run-package-config-smoke.sh \
  --prefix /tmp/conflux-install \
  --components 'core;json;file_io_sync'
```

Mock-liburing header-interface installs publish only liburing-free package
components: `core`, `types`, `json`, and `file_io_sync`. Runtime-facing package
components, including `runtime`, `http`, async file/socket I/O, and HTTP
server/client surfaces, require a real `liburing` package at producer configure
time and are consumed through the separate runtime smoke lane.

## Public component map

| Component | CMake target | Primary imports | Contract docs / examples |
|---|---|---|---|
| `core` | `conflux::core` | `conflux.types` via `conflux::types` | `docs/project-policy.md` |
| `types` | `conflux::types` | `conflux.types` | `tests/caps_test.cxx`, `tests/smoke.cxx` |
| `utils` | `conflux::utils` | `conflux.utils` | `tests/utils_test.cxx` |
| `net_config` | `conflux::net_config` | `conflux.net.config` | `docs/http-server-api.md`, `tests/config_test.cxx` |
| `net_cancel` | `conflux::net_cancel` | `conflux.net.cancel` | `docs/execution-model.md`, `tests/socket_task_ring_test.cxx` |
| `net_tls` | `conflux::net_tls` | `conflux.net.tls` | `tests/tls_external.cxx` |
| `net_io_buffer` | `conflux::net_io_buffer` | `conflux.net.io_buffer` | `tests/http_core_test.cxx` |
| `crypto` | `conflux::crypto` | `conflux.crypto` | `examples/advanced/crypto_sealing.cxx`, `tests/crypto_test.cxx` |
| `json_boundary` | `conflux::json_boundary` | `conflux.json.boundary` | `docs/json-boundary-guide.md` |
| `json` | `conflux::json` | `conflux.json` | `docs/json-api.md`, `docs/json-cookbook.md`, `examples/advanced/json.cxx` |
| `json_native_provider` | `conflux::json_native_provider` | `conflux.json.native_provider` | `docs/json-boundary-guide.md` |
| `json_file` | `conflux::json_file` | `conflux.json.file` | `docs/json-api.md`, `tests/json_file_test.cxx` |
| `json_reflect` | `conflux::json_reflect` | `conflux.json.reflect` | `docs/json-reflect.md` |
| `json_reflect_provider` | `conflux::json_reflect_provider` | `conflux.json.reflect_provider` | `docs/json-reflect.md` |
| `work` | `conflux::work` | `conflux.work`, `conflux.work.root`, `conflux.work.carrier.*` | `docs/conflux-work-root-api.md`, `docs/conflux-work-carrier-api.md` |
| `uring` | `conflux::uring` | `conflux.uring`, `conflux.uring.flow`, `conflux.uring.completion`, `conflux.uring.handle` | `docs/io_uring_direct_file_flow_design.md` |
| `uring_timeout` | `conflux::uring_timeout` | `conflux.uring.timeout` | `tests/uring_flow_test.cxx` |
| `file_io_sync` | `conflux::file_io_sync` | `conflux.file_io_sync` | `examples/advanced/file_io.cxx`, `tests/file_io_sync_test.cxx` |
| `file_map` | `conflux::file_map` | `conflux.file_map` | `tests/file_io_sync_test.cxx` |
| `file_io` | `conflux::file_io` | `conflux.file_io` umbrella plus `conflux.file_io.buffers`, `.pipe_pool`, `.reader`, `.iopoll`, `.driver` leaf modules | `examples/advanced/file_io.cxx`, `tests/file_io_test.cxx` |
| `file_watch` | `conflux::file_watch` | `conflux.file_watch` | `src/file_watch.cxx` |
| `template` | `conflux::template` | `conflux.templates` | `examples/advanced/template_pages.cxx`, `tests/template_test.cxx` |
| `template_watch` | `conflux::template_watch` | `conflux.templates.watch` | `examples/advanced/template_pages.cxx` |
| `socket_io` | `conflux::socket_io` | `conflux.socket_io`, `conflux.socket_io.coro`, `conflux.socket_io.blocking` | `tests/socket_task_ring_test.cxx`, `tests/tcp_listener_test.cxx` |
| `dns` | `conflux::dns` | `conflux.net.dns` | `tests/dns_codec_test.cxx`, `tests/dns_resolver_test.cxx` |
| `dns_bridge` | `conflux::dns_bridge` | `conflux.dns_bridge` HTTP-client DNS bridge provider | `examples/advanced/http_client_builder.cxx` |
| `process` | `conflux::process` | `conflux.process` | `examples/advanced/process_run.cxx`, `tests/process_test.cxx` |
| `pg` | `conflux::pg` | `conflux.pg` PostgreSQL API | `docs/db-api.md`, `examples/advanced/db_basic.cxx`, `examples/advanced/db_pool.cxx` |
| `db` | `conflux::db` | compatibility re-export of PostgreSQL API; not advertised for new public code | `docs/db-api.md` |
| `smtp` | `conflux::smtp` | `conflux.net.smtp` | `tests/smtp_test.cxx` |
| `umbrella` | `conflux::umbrella` | `conflux` | `README.md` |


Stage 1 profile rule of thumb: normal app/JSON facades are `curated`; stable extension/provider/customization modules are `extended`; raw runtime, protocol, parser, socket/file async I/O, and helper internals are `complete` or explicit-only. Detailed drift checking is deferred to the manifest stage.

## HTTP component map

| Component | CMake target | Primary imports | Contract docs / examples |
|---|---|---|---|
| `http_parse_helpers` | `conflux::http_parse_helpers` | `conflux.net.http.parse_helpers` | `tests/http_server_helpers_test.cxx` |
| `http_core` | `conflux::http_core` | `conflux.net.http.types`, `conflux.net.http.request` (`ClientRequest`), `conflux.net.http.server_types` (`http::Request`/`http::OwnedRequest`/`http::RequestView`) | `docs/conflux-http-client-api.md`, `docs/http-server-api.md` |
| `http_response` | `conflux::http_response` | `conflux.net.http.response` | `tests/http_response_test.cxx` |
| `http_json` | `conflux::http_json` | `conflux.net.http.json` | `docs/json-boundary-guide.md`, `tests/http_json_test.cxx` |
| `http_response_json` | `conflux::http_response_json` | `conflux.net.http.response_json` | `docs/json-boundary-guide.md` |
| `http_app_json` | `conflux::http_app_json` | `conflux.net.http.app_json` | `docs/json-boundary-guide.md`, `examples/advanced/manual_json_members.cxx`, `examples/advanced/explicit_offload.cxx` |
| `http_native_json` | `conflux::http_native_json` | `conflux.net.http.native_json` | `docs/json-boundary-guide.md`, `examples/advanced/manual_json_members.cxx`, `examples/advanced/http_client_json.cxx` |
| `http_router` | `conflux::http_router` | `conflux.net.router` | `docs/http-server-api.md`, `examples/hello.cxx` |
| `router_match` | `conflux::router_match` | `conflux.net.router_match` | `tests/http_core_test.cxx` |
| `router_dispatch` | `conflux::router_dispatch` | `conflux.net.router_dispatch` | `docs/naming-audit.md` |
| `router_static` | `conflux::router_static` | `conflux.net.router_static` | `tests/http_core_test.cxx` |
| `http_server_helpers` | `conflux::http_server_helpers` | `conflux.net.http_server_helpers` | `tests/http_server_helpers_test.cxx` |
| `http_server_config` | `conflux::http_server_config` | `conflux.net.http_server_config` | `docs/http-server-api.md`, `tests/config_test.cxx` |
| `http_server` | `conflux::http_server` | `conflux.net.http_server` | `docs/http-server-api.md`, `examples/hello.cxx` |
| `http_app` | `conflux::http_app` | `conflux.net.app` | `docs/http-server-api.md` |
| `http_client` | `conflux::http_client` | first-contact `conflux.net.http.client`, lower-level `conflux.net.client`, and HTTP/1 wire helpers in `conflux.net.client_wire` | `docs/conflux-http-client-api.md`, `examples/http_client.cxx`, `examples/advanced/http_client_json.cxx` |
| `http` | `conflux::http` | `conflux.net.http` umbrella plus first-contact `conflux.net.http.server` and `conflux.net.http.client` | `docs/http-server-api.md`, `docs/conflux-http-client-api.md` |
| `http_static_core` | `conflux::http_static_core` | `conflux.net.http.static_core` | `examples/static.cxx`, `tests/file_io_http_e2e.cxx` |
| `http_static` | `conflux::http_static` | `conflux.net.http.static_files` | `examples/static.cxx` |
| `http_static_async` | `conflux::http_static_async` | `conflux.net.http.static_async` | `examples/static.cxx` |
| `http_realtime` | `conflux::http_realtime` | `conflux.net.http.realtime` | `examples/sse.cxx` |
| `http_policy` | `conflux::http_policy` | `conflux.net.cache_control`, `conflux.net.cors`, `conflux.net.etag`, `conflux.net.forwarded`, `conflux.net.ip_filter`, `conflux.net.rate_limit`, `conflux.net.redirect`, `conflux.net.response_cache`, `conflux.net.security`, `conflux.net.trailing_slash` | `docs/auth-rate-limit-hooks.md`, `docs/http-security-corpus.md` |
| `http_auth` | `conflux::http_auth` | `conflux.net.auth`, `conflux.net.csrf`, `conflux.net.cookie_signing`, `conflux.net.password_hash`; `conflux.net.jwt` when TLS is built | `docs/auth-password-hashing.md`, `docs/auth-session-token-audit.md` |
| `http_observability` | `conflux::http_observability` | `conflux.net.observability`, `conflux.net.structured_log`, `conflux.net.tracing`, `conflux.net.request_id`; `conflux.net.metrics` when metrics are enabled | `examples/advanced/http_observability.cxx` |
| `http_compression` | `conflux::http_compression` | `conflux.net.compress` and enabled backend modules | `examples/gzip.cxx` |
| `http_openapi` | `conflux::http_openapi` | `conflux.net.openapi` | `examples/advanced/vhost_openapi.cxx` |
| `http_vhost` | `conflux::http_vhost` | `conflux.net.vhost` | `examples/advanced/vhost_openapi.cxx` |
| `http_async_client` | `conflux::http_async_client` | `conflux.net.async_client` | `docs/conflux-http-client-api.md`, `examples/advanced/http_client_builder.cxx` |
| `http_proxy` | `conflux::http_proxy` | `conflux.net.proxy` | `tests/http_e2e.cxx` |
| `http1` | `conflux::http1` | `conflux.net.http1_parser` | `docs/http-security-corpus.md`, `fuzz/fuzz_http1_parser.cxx` |
| `http2` | `conflux::http2` | `conflux.net.http2` | `tests/h2_external.cxx` |
| `http3` | `conflux::http3` | `conflux.net.http3` | `examples/advanced/h3_server.cxx`, `examples/advanced/h3_probe.cxx` |
| `http_protocol` | `conflux::http_protocol` | `conflux.net.http.protocol` | `tests/http_core_test.cxx` |

## Exported support targets

Targets whose component names start with `_` are exported because static-library
link interfaces may need them. They are not intended as public starting points:

| Component | Export target | Purpose |
|---|---|---|
| `_options` | private `_options` export | propagated compile options/definitions |
| `_direct_slot_pool` | private `_direct_slot_pool` export | HTTP/runtime direct-slot helper |
| `_simd_runtime` | private `_simd_runtime` export | private selected SIMD backend symbols for static-library link interfaces |
