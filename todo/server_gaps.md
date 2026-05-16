# Server / Framework Gaps

## Immediate

- [x] Direct-accept: set `tcp_nodelay_once` in `http_server.cxx` `DirectTcpAcceptSetup` (normal-accept already sets it)
- [x] JSON docs: sync `docs/json-api.md` field names with current `JsonError` / error-code names
- [x] JSON arena: `JsonArena::parse_borrowed_into(string_view)` and `parse_moved_into(string&&)` landed
- [x] JSON hash index: allocate via PMR resource, not global `::operator new` — landed in `4105bb2`
- [x] Ring thread affinity: add `ring_core` / `worker_core_base` config fields, default disabled
- [x] Busy poll: add `busy_poll_us` / `prefer_busy_poll` config fields, default disabled
- [x] Current regression: `http.cq_overflow: non-UB shutdown under small-CQ flood` aborts in `tests/http_overflow_stress_tests` on `debug-clang-libcxx`; fixed by routing HTTP recovery/append discard paths through the non-asserting buffer-ring CQE decoder.

## Medium

- [x] HTTP send path: registered send-buffer pool for headers/small bodies — landed (`FixedBufferPool` send buffers, `submit_send_fixed_borrowed` path for small plain responses)
- [x] HTTP send path: `SEND_ZC` integration with notification-CQE tracking, capability-gated — landed (`queue_send_mapped` uses `submit_send_zc_borrowed` above threshold, `IORING_CQE_F_NOTIF` handling, send-zc counters)
- [x] HTTP send path edge cases: mapped-file header+body now splits header send from large-body SEND_ZC, and `send_zc_bench` covers the adaptive threshold across plain/mapped bodies at the boundary sizes. TLS path still cannot use SEND_ZC directly. Note: mapped cache (when landed) removes repeated open/stat/mmap/close/munmap for hot static files but does not change the send path itself
- [x] O_TMPFILE atomic publish: sync path was already staged; async `FileReader::atomic_write_async()` now validates/splits contained paths, stages in the final parent dir, falls back to named temp when O_TMPFILE is unsupported, links staging + rename/renameat2, cleans staging on failure, and fsyncs the final parent dir.
- [x] Root `Task<T>`: add optional alloc counters for frames/control-blocks (`CONFLUX_WORK_ALLOC_STATS`)
- [x] Root `Task<T>`: pool `ControlBlockModel<T>` after measuring allocation counters — `work.root` now uses a pooled `std::pmr::synchronized_pool_resource` for `ControlBlockModel<T>` allocations, while keeping the existing allocation counters and control-flow semantics intact
- [x] JSON: true incremental `JsonStreamReader::feed(span<byte>)` / events API — `JsonStreamReader` now has feed/close/next coverage in `tests/json_test.cxx` and public docs in `docs/json-api.md`.
- [x] Router: `ContextHandler` / `ContextMiddleware` context-route dispatch — landed in `844b8dc`; the transitional dispatcher name is tracked in `docs/naming-audit.md`
- [x] Router: static-assert diagnostics for handler/middleware return types — landed (8+ static_asserts with clear error messages in router.cxx)
- [x] Execution model: HTTP handlers run on ring threads; all tasks run on an executor; hidden sync-handler auto-offload is not a planned fix. Public naming model is `blocking_*` for raw blocking syscall-style helpers, `sync_*` for executor-owned non-coroutine chains, and `async_*` for coroutine APIs.
- [x] Router: formal named public concepts (`HandlerResult`, `RouteHandler`, `ContextHandlerFunction`, `Middleware`) — exported and wired into `add_context`
- [x] io_uring init: adaptive flag fallback on `EINVAL` (strip `CQE_MIXED` → `NO_SQARRAY` → `TASKRUN_FLAG` → `DEFER_TASKRUN` → `SINGLE_ISSUER`, log final set)

- [x] Shutdown: force-close `close_after_send` connections that stall (send CQE never completes because peer stopped draining) — `handle_shutdown()` now stamps a shutdown deadline for queued sends and `handle_timer()` force-closes overdue connections instead of waiting indefinitely for send completion

## Test coverage gap pass (2026-05-14)

Implemented in this patch:

- [x] JSON arena borrowed-input semantics: `parse_borrowed_into(string_view)` now has a regression test proving unescaped strings remain borrowed views into caller storage.
- [x] JSON arena moved-input semantics: `parse_moved_into(string&&)` now has a regression test proving the arena-owned input survives caller-scope exit and BOM stripping.
- [x] Config parser coverage for implemented perf/isolation knobs: `busy_poll_us`, `ring_core`, `worker_core_base`, `prefer_busy_poll`, `direct_accept`, `cmd_sock_setsockopt`, `auto_recv_arm_policy`, `cqe_mixed`, and `submit_all`.
- [x] Config helper coverage for implemented observability text: `build_uring_flags`, `setup_flags_str`, `flags_str`, and `wq_fd_for_ring`.

Covered by follow-up deterministic seams:

- [x] Direct-accept `TCP_NODELAY` SQE emission from `queue_direct_accept_setup`; covered by direct-accept SQE inspection seam.
- [x] Direct-accept busy-poll / prefer-busy-poll SQE chain shape; covered by direct-accept SQE inspection seam.

Still implemented but not directly covered by deterministic tests:

- [ ] Ring-thread `sched_setaffinity` and `IORING_REGISTER_IOWQ_AFF` application; needs syscall injection or observable thread/affinity capture.
- [x] Adaptive `io_uring_queue_init_params(EINVAL)` fallback strip order; extracted to `next_uring_setup_flag_to_strip()` in `http_server_config.cxx` and covered by deterministic unit tests.
- [x] `SEND_ZC` notification-CQE lifecycle: `observe_send_zc_cqe(...)` provides a deterministic CQE transition seam, and `conflux_send_zc_lifecycle_tests` covers data+notification, no-notification, copied-notification adaptive disable, ENOMEM, and close-after-notification paths.
- [x] `JsonArena` PMR hash-index allocation source; `JsonArenaOptions::hash_index_resource` now lets tests inject a counting resource, and `warm_member_index()` is covered without relying on global `new`.

## Test coverage gap pass 2 (2026-05-14)

Implemented in this patch:

- [x] HTTP core URL parsing: direct tests now cover scheme normalization, default ports, bare query handling, IPv6 literals, `str()` reconstruction, query-param percent encoding, and distinct `UrlErrorKind` failures.
- [x] HTTP request builder: direct tests now cover Basic/Bearer auth helpers, JSON/form body helpers, `clear_body()`, conditional request date formatting, redirect/verify/SNI knobs, and query encoding.
- [x] HTTP server request/view ownership boundary: `HttpRequestView::to_owned()` now has a regression test proving borrowed uploaded-file metadata/data are copied before caller storage changes.
- [x] HTTP response helpers: direct tests now cover HTML escaping in generated error bodies, `Set-Cookie` formatting, `append_vary()` de-dup/`*` semantics, explicit `content_length_hint`, body extraction, redirects, Allow, WWW-Authenticate, and gateway error helpers.

Still implemented but not directly covered by deterministic tests after this pass:

- [x] HTTP client redirect execution path for relative vs absolute redirects, redirect-limit exhaustion, and sensitive header handling across host changes; now covered by sync/async E2E tests in pass 7.
- [x] HTTP request body double-set debug assertion path; covered in pass 5 by `http_request_assert_probe` + `conflux_http_core_tests`.

## Test coverage gap pass 3 (2026-05-14)

Implemented in this patch:

- [x] HTTP server helper response formatting: deterministic tests now cover invalid reason/header/cookie filtering, framing-header suppression, `Alt-Svc`, close/keep-alive emission, and body suppression for 204/304/HEAD responses.
- [x] HTTP server helper parsing primitives: deterministic tests now cover header-token validation, parameter extraction, cookie parsing, `Connection` token matching, `Expect` parsing, and strict `Transfer-Encoding: chunked` validation.
- [x] HTTP request body helper parsing: deterministic tests now cover URL-encoded form decoding, multipart text/file extraction, complete chunked decoding, incremental chunked decoding, incomplete input, malformed chunks, and too-large bodies.

Still implemented but not directly covered by deterministic tests after pass 3:

- [x] HTTP parser/helper duplicate implementation drift: the exported `conflux.net.http_server_helpers` module is the source of truth for request/body parsing helpers, and the dead `#if 0` chunked-decode block in `src/net/http_server_impl.cxx` has been removed.
- [x] HTTP client redirect execution path for relative vs absolute redirects, redirect-limit exhaustion, and sensitive header handling across host changes; now covered by sync/async E2E tests in pass 7.
- [x] HTTP request body double-set debug assertion path; covered in pass 5 by `http_request_assert_probe` + `conflux_http_core_tests`.

## Test coverage gap pass 4 (2026-05-14)

Implemented in this patch:

- [x] HTTP response non-text body-kind transitions: direct tests now cover SSE, WebSocket upgrade, mapped-file, streamed-file, and deferred payload setters/accessors/take helpers, plus text reset through `text_body_mut()`.
- [x] `DeferredResponse` eventfd wake-read behavior: direct tests now drain the eventfd after `complete()` and `expire_if_past_deadline()`, verify single-wake first-result semantics, and verify 504 materialization on expiry.

Still implemented but not directly covered by deterministic tests after pass 4:

- [x] HTTP parser/helper duplicate implementation drift: the exported `conflux.net.http_server_helpers` module is the source of truth for request/body parsing helpers, and the dead `#if 0` chunked-decode block in `src/net/http_server_impl.cxx` has been removed.
- [x] HTTP client redirect execution path for relative vs absolute redirects, redirect-limit exhaustion, and sensitive header handling across host changes; now covered by sync/async E2E tests in pass 7.

## Test coverage gap pass 5 (2026-05-14)

Implemented in this patch:

- [x] HTTP request body double-set debug assertion path: `http_request_assert_probe` now exercises `body()` followed by `body_view()`, `body_json_raw()`, and `body_form()`; `conflux_http_core_tests` verifies all three debug builds exit through the assert sentinel.

Still implemented but not directly covered by deterministic tests after pass 5:

- [x] HTTP parser/helper duplicate implementation drift: the exported `conflux.net.http_server_helpers` module is the source of truth for request/body parsing helpers, and the dead `#if 0` chunked-decode block in `src/net/http_server_impl.cxx` has been removed.

## Test coverage gap pass 6 (2026-05-14)

Implemented in this patch:

- [x] HTTP response factory matrix: `conflux_http_response_tests` now covers the implemented `html`, `json`, `forbidden`, `unprocessable_entity`, `uri_too_long`, `header_fields_too_large`, `content_too_large`, `no_content`, `sse`, and `deferred` factories instead of only the smaller redirect/auth/gateway subset.
- [x] WebSocket upgrade wire formatting: `conflux_http_server_helpers_tests` now verifies `format_response()` emits the implemented 101 handshake fast path and does not serialize unrelated normal response headers/body.

Still implemented but not directly covered by deterministic tests after pass 6:

- [x] HTTP parser/helper duplicate implementation drift: the exported `conflux.net.http_server_helpers` module is the source of truth for request/body parsing helpers, and the dead `#if 0` chunked-decode block in `src/net/http_server_impl.cxx` has been removed.

## Test coverage gap pass 7 (2026-05-14)

Implemented in this patch:

- [x] HTTP client redirect following: `HttpRequest::Builder::follow_redirects()` is now consumed by the sync and async clients. Relative and absolute redirects, redirect-limit exhaustion, and cross-origin sensitive-header stripping are covered by E2E tests.

## Architecture (dedicated branches)

- [x] Dedicated `IOPOLL` ring for O_DIRECT file I/O, separate from network ring — landed as `IopollStorageRing` / `IopollFileReader` for storage-only O_DIRECT reads on a dedicated `IORING_SETUP_IOPOLL` ring.
- [ ] Local worker queues: profile mutex contention first, then Chase-Lev if warranted (global injection has MPMC ring; per-worker local queues + stealing still use `std::mutex`)
- [ ] `admission_mtx_` in `work.cxx`: profile under high-RPS, replace with atomic if bottleneck
- [x] Ring metrics: expose `sq_dropped`, `cq_overflow`, `accepted_direct_failures`, `zc_notif_pending`, recv-bundle stats, SEND_ZC usage/copy/adaptive-disable counters as observable counters — added `HttpServer::metrics()` snapshot; CQ overflow auto-grow remains separate.
- [x] io_uring startup log: make setup flag fallback stripping exact (log which of `NO_SQARRAY`, `SUBMIT_ALL`, `CQE_MIXED` were stripped; now logs requested/active/stripped setup-flag sets)

## Packaging / release blockers

- [x] CMake install + exported package config + namespaced targets. Foundation: `conflux::core`, `conflux::utils`, `conflux::net_config`, `conflux::file_io_sync`, `conflux::file_map`, `conflux::runtime`, `conflux::file_io`, `conflux::socket_io`, `conflux::dns`, `conflux::crypto`, `conflux::json`, `conflux::json_file`, `conflux::template`, `conflux::template_watch`. HTTP: `conflux::http_core`, `conflux::http_router`, `conflux::http_server`, `conflux::http_static_core`, `conflux::http_static_async`, `conflux::http_auth`, `conflux::http_json`, `conflux::http_response_json`, `conflux::http_app_json`, `conflux::http_native_json`, `conflux::http_policy`, `conflux::http_observability`, `conflux::http_openapi`, `conflux::http_realtime`, `conflux::http_vhost`, `conflux::http_client_sync`, `conflux::http_client_async`
  - [x] First install/export slice: export existing split targets with stable `conflux::...` names and generated `conflux-config.cmake`.
  - [x] Template split slice: export `conflux::template` and `conflux::template_watch` as real module targets; remove template/file-watch module sources from the HTTP monolith.
  - [x] HTTP JSON/core split slice: export `conflux::http_core` (`http.types` + `http.request`), `conflux::http_json`, `conflux::http_response_json`, `conflux::http_app_json`, and `conflux::http_native_json`; remove those module sources from the HTTP monolith. Follow-up slices below cover `json_file`, router/server/static/client, and the other HTTP feature targets.
  - [x] Package export placement slice: install/export/package-config generation now lives outside `CONFLUX_WANT_HTTP_SERVER`, so core/json-only builds still expose `conflux::...` package targets instead of requiring the HTTP monolith.
  - [x] Support module split slice: `src/utils.cxx` and `src/net/config.cxx` now build as `conflux::utils` and `conflux::net_config`; the HTTP monolith links/imports them instead of compiling those module units directly. This is the prerequisite for a clean router/server split.
  - [x] TLS prerequisite split slice: export `conflux::net_cancel` and `conflux::net_tls`; remove `cancel.cxx`, `cancel_impl.cxx`, and `tls.cxx` from direct HTTP monolith compilation.
  - [x] HTTP router/middleware split slice: export `conflux::http_router`, `conflux::http_policy`, `conflux::http_auth`, `conflux::http_observability`, `conflux::http_openapi`, and `conflux::http_vhost`; remove their module sources from direct HTTP monolith compilation.
  - [x] HTTP client/compression/proxy split slice: export `conflux::http_client`, `conflux::http_async_client`, `conflux::http_compression`, `conflux::http_proxy`, and `conflux::smtp`; remove their module sources and private impl units from direct HTTP monolith compilation. Follow-up slices below cover server/static/realtime/protocol/umbrella targets.
  - [x] HTTP protocol/server/app/umbrella split slice: export `conflux::process`, `conflux::net_io_buffer`, `conflux::http_protocol`, `conflux::http_server`, `conflux::http_app`, and `conflux::http`; remove `process.cxx`, `io_buffer.cxx`, `http1_parser.cxx`, optional protocol modules, `http_server.cxx`/impl, `app.cxx`, and `http.cxx` from direct aggregate compilation. Follow-up static/realtime/response slices below complete the standalone target boundary.
  - [x] Optional protocol target split slice: export `conflux::http1`, `conflux::http2`, and `conflux::http3` as real module targets under `conflux::http_protocol`; tie nghttp2/ngtcp2/nghttp3 package dependency restoration to those optional targets.
  - [x] Static/realtime surface slice: export `conflux::http_static` (`StaticOptions`), move server request/view/callback vocabulary into `conflux::http_core`, and export `conflux::http_realtime` (SSE + WebSocket surfaces); `conflux.net.router` re-exports these for source compatibility while static implementation internals remain queued for a later split.
  - [x] Static implementation split slice: export `conflux::http_response`, `conflux::http_static_core`, and `conflux::http_static_async`; move HTTP response/deferred body vocabulary, static path normalization/cache store, static root-dir lifetime, contained open/probe helpers, static GET/PUT/DELETE execution, and async file helper coroutines out of router-owned module units.
  - [x] Dependency-edge cleanup slice: stale `socket_io -> file_io`, `net.client -> file_io`, and `body_json(NodeRef)` proposal edges are gone; higher-level targets/tests/benches no longer repeat direct `PkgConfig::LIBURING` links already propagated by lower-level runtime/file/socket targets.
  - [x] DB optional component export: `conflux_db` installs/exports as `conflux::db` when libpq support is enabled.
- [ ] CI: examples compile gate, sanitizer correctness lane, runnable fuzz-smoke lane, and optional JSONTestSuite gate exist; still need an explicit bench regression budget.
- [x] Docs: versioning/semver policy, security disclosure, supported compiler+kernel matrix — added `docs/project-policy.md` and linked it from README/pre-v1 contract
- [x] HTTP security corpus: smuggling, duplicate Content-Length, TE ambiguity, Host ambiguity, chunk edge cases — documented in `docs/http-security-corpus.md`; raw-wire E2E cases added for Host and CL+TE smuggling

## Do not implement

- ORM, DI container, code generator
- Default-on ring/worker pinning or `MAP_HUGETLB`
- Full P2300 rewrite before measuring root `Task<T>` cost
- `RECV_ZC` production integration (kernel behavior not stable)
