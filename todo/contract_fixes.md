# Contract / Design-Rule Fixes

## Hard breaks

- [x] CMake: public component split per modular proposal is implemented. Foundation targets exist for `conflux::core`, `conflux::utils`, `conflux::net_config`, `conflux::file_io_sync`, `conflux::file_map`, `conflux::runtime`, `conflux::file_io`, `conflux::socket_io`, `conflux::dns`, `conflux::crypto`, `conflux::json`, `conflux::json_file`, `conflux::template`, and `conflux::template_watch`. HTTP/package targets now cover core, router, server, static, auth, JSON, policy, observability, OpenAPI, realtime, vhost, client, async-client, protocol, response, and app JSON slices with install/export/namespaced package targets.
  - [x] First packaging slice: existing modular targets now have build-tree aliases and install/export names (`conflux::core`, `conflux::types`, `conflux::runtime`, `conflux::file_io_sync`, `conflux::file_map`, `conflux::file_io`, `conflux::socket_io`, `conflux::dns`, `conflux::crypto`, `conflux::json`, `conflux::db`, `conflux::http_server`); package config restores required PkgConfig/OpenSSL/ZLIB deps.
  - [x] Template split slice: `src/template.cxx` and `src/file_watch.cxx` are separate install/export module targets (`conflux::template`, `conflux::template_watch`) instead of being compiled into the HTTP monolith; umbrella `conflux` re-exports them only when present.
  - [x] HTTP JSON/core split slice: `src/net/http_types.cxx` and `src/net/http_request.cxx` now build as `conflux::http_core`; `src/net/http_json.cxx` now builds as `conflux::http_json`; the monolith links/imports them instead of compiling those module units directly. Follow-up slices below split router/server/static/client/policy/auth/observability/OpenAPI/realtime/vhost targets.
  - [x] Package export placement slice: install/export/package-config generation now lives outside `CONFLUX_WANT_HTTP_SERVER`, so core/json-only builds still expose `conflux::...` package targets instead of requiring the HTTP monolith.
  - [x] File sync/map independence slice: `conflux_file_io_sync` and `conflux_file_map` are now created from their own component flags outside the runtime-gated block; `core + file_io_sync/file_map` presets no longer require `liburing` target creation.
  - [x] JSON file convenience slice: `conflux::json_file` / `conflux.json.file` adds sync `parse_file_at_sync` helpers on top of `json + file_io_sync`; `conflux::json` remains free of file I/O, mmap, logging globals, and io_uring.
  - [x] Support module split slice: `src/utils.cxx` and `src/net/config.cxx` now build as `conflux::utils` and `conflux::net_config`; the HTTP monolith links/imports them instead of compiling those module units directly. This is the prerequisite for a clean router/server split.
  - [x] TLS prerequisite split slice: `src/net/cancel.cxx`/`cancel_impl.cxx` now build as `conflux::net_cancel`, and `src/net/tls.cxx` builds as `conflux::net_tls` when TLS is enabled; the HTTP monolith links/imports those module targets instead of compiling TLS/cancel directly. This removes the immediate TLS blocker for a later router/server split.
  - [x] HTTP router/middleware split slice: `src/net/router.cxx`/`router_impl.cxx` now build as `conflux::http_router`; policy/cache/redirect helpers build as `conflux::http_policy`; auth/cookie/CSRF/JWT build as `conflux::http_auth`; request id/logging/tracing/metrics build as `conflux::http_observability`; OpenAPI and vhost build as `conflux::http_openapi` and `conflux::http_vhost`.
  - [x] HTTP client/compression/proxy split slice: `src/net/client.cxx`, `client_async.cxx`/`client_async_impl.cxx`, `compress.cxx` plus enabled gzip backends, `proxy.cxx`/`proxy_impl.cxx`, `client_dns_bridge.cxx`, and `smtp.cxx` now build as separate exported targets (`conflux::http_client`, `conflux::http_async_client`, `conflux::http_compression`, `conflux::http_proxy`, `conflux::smtp`, internal `conflux::_dns_bridge`) instead of being compiled directly into the HTTP monolith. Follow-up slices below split server/static/realtime/protocol/umbrella targets.
  - [x] HTTP protocol/server/app/umbrella split slice: `src/process.cxx`, `src/net/io_buffer.cxx`, `src/net/http1_parser.cxx`, optional `http2.cxx`/`http3.cxx`, `src/net/http_server.cxx`/`http_server_impl.cxx`, `src/net/app.cxx`, and `src/net/http.cxx` now build as exported targets (`conflux::process`, `conflux::net_io_buffer`, `conflux::http_protocol`, `conflux::http_server`, `conflux::http_app`, `conflux::http`). The legacy aggregate `conflux` target now primarily compiles `src/conflux.cxx` and links the component graph; follow-up slices split static/realtime/exported response surfaces.
  - [x] Static/realtime surface slice: `src/net/static.cxx` now exports `conflux.net.http.static_files` / `conflux::http_static` for `StaticOptions`; `src/net/server_types.cxx` exports server request/view/callback vocabulary through `conflux::http_core`; and `src/net/realtime.cxx` exports `conflux.net.http.realtime` / `conflux::http_realtime` for SSE plus WebSocket surfaces. Router re-exports these modules while static implementation internals remain queued.
  - [x] Static implementation split slice: `src/net/response.cxx` now exports `conflux.net.http.response` / `conflux::http_response`; `src/net/static_core.cxx` exports static request/cache/path-normalization internals; and `src/net/static_async.cxx` owns static root-dir lifetime, contained open/probe helpers, GET/PUT/DELETE execution paths, and async file helper coroutines. Router still registers the routes, but no longer owns heavy static-file execution/cache helpers or links file_io directly for static internals.
  - [x] Dependency-edge cleanup slice: stale `socket_io -> file_io`, `net.client -> file_io`, and `body_json(NodeRef)` proposal edges are gone; higher-level component/example/test/benchmark targets no longer repeat direct `PkgConfig::LIBURING` links already propagated by lower-level runtime/file/socket targets.
- [x] Execution model documentation: HTTP server handlers intentionally run on io_uring ring threads; all tasks run on an executor; do not add hidden auto-offload for arbitrary sync handlers. Public naming model documented: `blocking_*` for raw blocking syscall-style helpers, `sync_*` for executor-owned non-coroutine chains, and `async_*` for coroutine APIs.
- [ ] Public API alias cleanup: remove exported shorthand aliases (`S`, `SV`, `SP`, `Opt`, etc. in `src/types.cxx:15-75`) from public signatures and use spelled-out standard vocabulary types at public boundaries.
  - Last before release. Lowest priority. Do not touch until the rest of the public surface work is complete.
  - Doc/example string replacements for the renamed public surface stay until the very end of the release prep, after the code surface is settled.
  - [x] Net config boundary slice: exported `Config`/`VirtualHost` string fields now use `std::string` and `std::vector<std::string>` instead of `S`/`V<S>` at the public boundary.
  - [x] HTTP server config helpers now return `std::string` instead of `S` at the public boundary.
  - [x] VHost router helpers now use `std::string`, `std::string_view`, `std::shared_ptr`, and `std::optional` at the public boundary.
  - [x] HTTP server helper exports now use spelled-out string types at the public boundary (`std::string`, `std::string_view`, `std::int64_t`).
  - [x] OpenAPI spec generator now returns `std::string` at the public boundary.
  - [x] HTTP/3 alt-svc helper now returns `std::string` at the public boundary.
  - [x] Cookie signing and WebSocket accept-key helpers now return `std::string` / `std::optional<std::string>`.
  - [x] Chunked-body helper now returns `std::optional<std::string>` at the public boundary.
  - [x] Gzip backend-name helper now returns `std::string_view` at the public boundary.
  - [x] JWT sign/decode helpers now use `std::string` at the public boundary.
  - [x] Static path normalization now returns `std::optional<std::string>` at the public boundary.
  - [x] Crypto helper exports now use spelled-out `std::string`, `std::string_view`, `std::span`, `std::array`, `std::vector`, and `std::expected` at the public boundary.
  - [x] Utility helper exports now use spelled-out string/view/span/optional/vector/pair/integer vocabulary at the public boundary.
  - [x] Process options/results and spawn/run helpers now use `std::filesystem::path`, `std::vector`, `std::string`, `std::string_view`, `std::optional`, `std::expected`, and `std::error_code` at the public boundary.
  - [x] File sync/map exported APIs now use spelled-out `std::uint*_t`, `std::string`, `std::string_view`, `std::span`, `std::optional`, and `std::expected` vocabulary at the public boundary.
  - [x] Async file I/O exported APIs now use spelled-out `std::uint*_t`, `std::size_t`, `std::string`, `std::span`, `std::vector`, `std::array`, `std::pair`, `std::optional`, `std::expected`, and `std::chrono` vocabulary at the public boundary.
  - [x] Socket I/O exported APIs now use spelled-out `std::uint*_t`, `std::size_t`, `std::string`, `std::span`, `std::vector`, `std::array`, `std::pair`, `std::optional`, `std::expected`, `std::function`, and `std::chrono` vocabulary at the public boundary.
- [ ] API naming pass: align public APIs with `blocking_*` / `sync_*` / `async_*`; reserve `blocking_*` for raw syscall-style helpers such as direct fd/process operations, use `sync_*` for executor-owned non-coroutine chains, and use `async_*` for coroutine APIs.
  - Keep doc/example replacement wording for this pass until the code rename lands; this is the final polish step, not the first change.
  - [x] Process executor-targeted helpers now use explicit async names: `spawn_async_in`, `run_async_in`, and `wait_async_in`.
- [x] JSON default path: `parse(string_view)` now aliases the borrowed/view path; explicit owning parse moved to `parse_copy(string_view|string&&)`; rvalue `parse(string&&)` is deleted; docs/examples promote `parse_view` as the primary fast API
- [x] JSON: add `JsonArena::parse_borrowed_into(string_view)` and `parse_moved_into(string&&)` — both landed; `parse_borrowed_into` reuses caller's buffer without copy, `parse_moved_into` takes ownership.
- [x] Core error type / `file_io_sync`: `IoError` is exported by `conflux.types`; `file_io_sync` no longer imports or links `conflux.uring.completion` / `conflux_uring`; `conflux::core` and `conflux::file_io_sync` are exported package components.
- [x] xxhash: resolve via `pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)` and link `PkgConfig::XXHASH` instead of a dangling raw `xxhash` target/name
- [x] JSON global state: document `CLocaleHolder` singleton as the only permitted process-lifetime singleton; design note added in `docs/json-design.md`

## Incomplete perf quick-wins

- [x] Direct-accept `TCP_NODELAY`: landed
- [x] `MADV_DONTFORK` for `FixedBufferPool` slabs: landed
- [x] Setup-flag fallback: landed (try-init/strip matrix, log requested vs active)
- [x] `NO_SQARRAY`: landed (default on, stripped on unsupported kernel)
- [x] `alignas(64)` on `Conn` and `Worker`: landed (`struct alignas(64) Conn`, `struct alignas(64) Worker`)
- [ ] `Ring` hot/cold field layout: `Ring` has no `alignas`; verify `Conn`/`Worker` field grouping with `perf c2c` before further padding
- [x] Root `Task<T>` allocation diagnostics: optional `CONFLUX_WORK_ALLOC_STATS` counters for control blocks and coroutine frames landed
- [x] Coroutine frame/control-block pool: landed pool targets `EagerChainPromise`, and `root::Task<T>::promise_type` / `ControlBlockModel<T>` now use process-lifetime synchronized pool resources while keeping allocation counters and task/control semantics intact

## Docs / API contract mismatches

- [x] `docs/json-api.md:49-51`: update limits to match code (`max_input = 128 MiB`, `max_string = 64 MiB`); docs still say 4 GiB / unlimited (`src/json.cxx:661-663`)
