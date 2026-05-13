# Server / Framework Gaps

## Immediate

- [x] Direct-accept: set `tcp_nodelay_once` in `http_server.cxx` `DirectTcpAcceptSetup` (normal-accept already sets it)
- [x] JSON docs: sync `docs/json-api.md` field names with current `JsonError` / error-code names
- [x] JSON arena: `JsonArena::parse_borrowed_into(string_view)` and `parse_moved_into(string&&)` landed
- [x] JSON hash index: allocate via PMR resource, not global `::operator new` — landed in `4105bb2`
- [x] Ring thread affinity: add `ring_core` / `worker_core_base` config fields, default disabled
- [x] Busy poll: add `busy_poll_us` / `prefer_busy_poll` config fields, default disabled

## Medium

- [x] HTTP send path: registered send-buffer pool for headers/small bodies — landed (`FixedBufferPool` send buffers, `submit_send_fixed_borrowed` path for small plain responses)
- [x] HTTP send path: `SEND_ZC` integration with notification-CQE tracking, capability-gated — landed (`queue_send_mapped` uses `submit_send_zc_borrowed` above threshold, `IORING_CQE_F_NOTIF` handling, send-zc counters)
- [ ] HTTP send path edge cases: mapped-file header+body still falls back to `writev`; TLS path cannot use SEND_ZC directly; adaptive threshold needs benchmark validation. Note: mapped cache (when landed) removes repeated open/stat/mmap/close/munmap for hot static files but does not change the send path itself
- [x] O_TMPFILE atomic publish: sync path was already staged; async `FileReader::atomic_write_async()` now validates/splits contained paths, stages in the final parent dir, falls back to named temp when O_TMPFILE is unsupported, links staging + rename/renameat2, cleans staging on failure, and fsyncs the final parent dir.
- [x] Root `Task<T>`: add optional alloc counters for frames/control-blocks (`CONFLUX_WORK_ALLOC_STATS`)
- [ ] Root `Task<T>`: pool `ControlBlockModel<T>` after measuring allocation counters
- [ ] JSON: true incremental `JsonStreamReader::feed(span<byte>)` / events API
- [x] Router: `ContextHandler` / `ContextMiddleware` / `dispatch_async()` — landed in `844b8dc`
- [x] Router: static-assert diagnostics for handler/middleware return types — landed (8+ static_asserts with clear error messages in router.cxx)
- [x] Router: formal named public concepts (`HandlerResult`, `RouteHandler`, `ContextHandlerFunction`, `Middleware`) — exported and wired into `add_context`
- [x] io_uring init: adaptive flag fallback on `EINVAL` (strip `CQE_MIXED` → `NO_SQARRAY` → `TASKRUN_FLAG` → `DEFER_TASKRUN` → `SINGLE_ISSUER`, log final set)

- [ ] Shutdown: force-close `close_after_send` connections that stall (send CQE never completes because peer stopped draining) — cancel in-flight send + immediate close after a shutdown timeout, rather than waiting indefinitely for send completion

## Architecture (dedicated branches)

- [ ] Dedicated `IOPOLL` ring for O_DIRECT file I/O, separate from network ring
- [ ] Local worker queues: profile mutex contention first, then Chase-Lev if warranted (global injection has MPMC ring; per-worker local queues + stealing still use `std::mutex`)
- [ ] `admission_mtx_` in `work.cxx`: profile under high-RPS, replace with atomic if bottleneck
- [x] Ring metrics: expose `sq_dropped`, `cq_overflow`, `accepted_direct_failures`, `zc_notif_pending`, recv-bundle stats, SEND_ZC usage/copy/adaptive-disable counters as observable counters — added `HttpServer::metrics()` snapshot; CQ overflow auto-grow remains separate.
- [x] io_uring startup log: make setup flag fallback stripping exact (log which of `NO_SQARRAY`, `SUBMIT_ALL`, `CQE_MIXED` were stripped; now logs requested/active/stripped setup-flag sets)

## Packaging / release blockers

- [ ] CMake install + exported package config + namespaced targets. Foundation: `conflux::core`, `conflux::utils`, `conflux::net_config`, `conflux::file_io_sync`, `conflux::file_map`, `conflux::runtime`, `conflux::file_io`, `conflux::socket_io`, `conflux::dns`, `conflux::crypto`, `conflux::json`, `conflux::json_file`, `conflux::template`, `conflux::template_watch`. HTTP: `conflux::http_core`, `conflux::http_router`, `conflux::http_server`, `conflux::http_static_core`, `conflux::http_static_async`, `conflux::http_auth`, `conflux::http_json`, `conflux::http_policy`, `conflux::http_observability`, `conflux::http_openapi`, `conflux::http_realtime`, `conflux::http_vhost`, `conflux::http_client_sync`, `conflux::http_client_async`
  - [x] First install/export slice: export existing split targets with stable `conflux::...` names and generated `conflux-config.cmake`.
  - [x] Template split slice: export `conflux::template` and `conflux::template_watch` as real module targets; remove template/file-watch module sources from the HTTP monolith.
  - [x] HTTP JSON/core split slice: export `conflux::http_core` (`http.types` + `http.request`) and `conflux::http_json`; remove those module sources from the HTTP monolith. Remaining work: split/export `json_file`, router/server/static/client and the other HTTP feature targets.
  - [x] Package export placement slice: install/export/package-config generation now lives outside `CONFLUX_WANT_HTTP_SERVER`, so core/json-only builds still expose `conflux::...` package targets instead of requiring the HTTP monolith.
  - [x] Support module split slice: `src/utils.cxx` and `src/net/config.cxx` now build as `conflux::utils` and `conflux::net_config`; the HTTP monolith links/imports them instead of compiling those module units directly. This is the prerequisite for a clean router/server split.
  - [x] TLS prerequisite split slice: export `conflux::net_cancel` and `conflux::net_tls`; remove `cancel.cxx`, `cancel_impl.cxx`, and `tls.cxx` from direct HTTP monolith compilation.
  - [x] HTTP router/middleware split slice: export `conflux::http_router`, `conflux::http_policy`, `conflux::http_auth`, `conflux::http_observability`, `conflux::http_openapi`, and `conflux::http_vhost`; remove their module sources from direct HTTP monolith compilation.
  - [x] HTTP client/compression/proxy split slice: export `conflux::http_client`, `conflux::http_async_client`, `conflux::http_compression`, `conflux::http_proxy`, and `conflux::smtp`; remove their module sources and private impl units from direct HTTP monolith compilation. Remaining work: split/export server/static/realtime/protocol/umbrella targets.
  - [x] HTTP protocol/server/app/umbrella split slice: export `conflux::process`, `conflux::net_io_buffer`, `conflux::http_protocol`, `conflux::http_server`, `conflux::http_app`, and `conflux::http`; remove `process.cxx`, `io_buffer.cxx`, `http1_parser.cxx`, optional protocol modules, `http_server.cxx`/impl, `app.cxx`, and `http.cxx` from direct aggregate compilation. Remaining work: static/realtime still live inside router/server internals and need a real module-boundary design before they can become standalone targets.
  - [x] Optional protocol target split slice: export `conflux::http1`, `conflux::http2`, and `conflux::http3` as real module targets under `conflux::http_protocol`; tie nghttp2/ngtcp2/nghttp3 package dependency restoration to those optional targets.
  - [x] Static/realtime surface slice: export `conflux::http_static` (`StaticOptions`), move server request/view/callback vocabulary into `conflux::http_core`, and export `conflux::http_realtime` (SSE + WebSocket surfaces); `conflux.net.router` re-exports these for source compatibility while static implementation internals remain queued for a later split.
  - [x] Static implementation split slice: export `conflux::http_response`, `conflux::http_static_core`, and `conflux::http_static_async`; move HTTP response/deferred body vocabulary, static path normalization/cache store, static GET execution, and async static PUT/DELETE helper coroutines out of router-owned module units.
- [ ] Split optional components: DB and any remaining static route registration internals. TLS, HTTP/2, HTTP/3, static option/core/async targets, SSE realtime types, and WebSocket realtime types now have separate link targets.
- [ ] CI: all examples compile, fuzz smokes, JSONTestSuite, ASan/UBSan lane, bench regression budget
- [x] Docs: versioning/semver policy, security disclosure, supported compiler+kernel matrix — added `docs/project-policy.md` and linked it from README/pre-v1 contract
- [x] HTTP security corpus: smuggling, duplicate Content-Length, TE ambiguity, Host ambiguity, chunk edge cases — documented in `docs/http-security-corpus.md`; raw-wire E2E cases added for Host and CL+TE smuggling

## Do not implement

- ORM, DI container, code generator
- Default-on ring/worker pinning or `MAP_HUGETLB`
- Full P2300 rewrite before measuring root `Task<T>` cost
- `RECV_ZC` production integration (kernel behavior not stable)
