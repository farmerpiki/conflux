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
- [x] HTTP send path edge cases: mapped-file header+body now splits header send from large-body SEND_ZC, and `send_zc_bench` covers the adaptive threshold across plain/mapped bodies at the boundary sizes. TLS path still cannot use SEND_ZC directly. Note: mapped cache (when landed) removes repeated open/stat/mmap/close/munmap for hot static files but does not change the send path itself
- [x] O_TMPFILE atomic publish: sync path was already staged; async `FileReader::atomic_write_async()` now validates/splits contained paths, stages in the final parent dir, falls back to named temp when O_TMPFILE is unsupported, links staging + rename/renameat2, cleans staging on failure, and fsyncs the final parent dir.
- [x] Root `Task<T>`: add optional alloc counters for frames/control-blocks (`CONFLUX_WORK_ALLOC_STATS`)
- [x] Root `Task<T>`: pool `ControlBlockModel<T>` after measuring allocation counters — `work.root` now uses a pooled `std::pmr::synchronized_pool_resource` for `ControlBlockModel<T>` allocations, while keeping the existing allocation counters and control-flow semantics intact
- [ ] JSON: true incremental `JsonStreamReader::feed(span<byte>)` / events API
- [x] Router: `ContextHandler` / `ContextMiddleware` / `dispatch_async()` — landed in `844b8dc`
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

Still implemented but not directly covered by deterministic tests:

- [ ] Direct-accept `TCP_NODELAY` SQE emission from `queue_direct_accept_setup`; needs either SQE-inspection seam or kernel-capability-gated direct-accept E2E.
- [ ] Direct-accept busy-poll / prefer-busy-poll SQE chain shape; same SQE-inspection seam as above.
- [ ] Ring-thread `sched_setaffinity` and `IORING_REGISTER_IOWQ_AFF` application; needs syscall injection or observable thread/affinity capture.
- [ ] Adaptive `io_uring_queue_init_params(EINVAL)` fallback strip order; implementation is inline in ring init and needs extraction to a pure helper or syscall seam before deterministic unit coverage.
- [ ] `SEND_ZC` notification-CQE lifecycle under a real kernel; benchmarks and metrics exist, but deterministic unit coverage still needs a CQE injection seam.
- [ ] `JsonArena` PMR hash-index allocation source; behavior is implemented, but proving “not global new” needs allocator instrumentation or a test-only resource hook.

## Test coverage gap pass 2 (2026-05-14)

Implemented in this patch:

- [x] HTTP core URL parsing: direct tests now cover scheme normalization, default ports, bare query handling, IPv6 literals, `str()` reconstruction, query-param percent encoding, and distinct `UrlErrorKind` failures.
- [x] HTTP request builder: direct tests now cover Basic/Bearer auth helpers, JSON/form body helpers, `clear_body()`, conditional request date formatting, redirect/verify/SNI knobs, and query encoding.
- [x] HTTP server request/view ownership boundary: `HttpRequestView::to_owned()` now has a regression test proving borrowed uploaded-file metadata/data are copied before caller storage changes.
- [x] HTTP response helpers: direct tests now cover HTML escaping in generated error bodies, `Set-Cookie` formatting, `append_vary()` de-dup/`*` semantics, explicit `content_length_hint`, body extraction, redirects, Allow, WWW-Authenticate, and gateway error helpers.

Still implemented but not directly covered by deterministic tests after this pass:

- [ ] HTTP client redirect execution path for relative vs absolute redirects, redirect-limit exhaustion, and sensitive header handling across host changes; builder knobs are covered, but network redirect behavior still needs a local two-server E2E.
- [ ] HTTP request body double-set debug assertion path; needs an assert-probe binary like the recv-bundle probes because release builds intentionally use last-wins behavior.

## Test coverage gap pass 3 (2026-05-14)

Implemented in this patch:

- [x] HTTP server helper response formatting: deterministic tests now cover invalid reason/header/cookie filtering, framing-header suppression, `Alt-Svc`, close/keep-alive emission, and body suppression for 204/304/HEAD responses.
- [x] HTTP server helper parsing primitives: deterministic tests now cover header-token validation, parameter extraction, cookie parsing, `Connection` token matching, `Expect` parsing, and strict `Transfer-Encoding: chunked` validation.
- [x] HTTP request body helper parsing: deterministic tests now cover URL-encoded form decoding, multipart text/file extraction, complete chunked decoding, incremental chunked decoding, incomplete input, malformed chunks, and too-large bodies.

Still implemented but not directly covered by deterministic tests after pass 3:

- [ ] HTTP parser/helper duplicate implementation drift: `http_server_impl.cxx` still has local copies of helper logic next to the exported `conflux.net.http_server_helpers` module; tests cover the exported helper API, but not that the legacy local copies stay byte-for-byte behavior-compatible until removed.
- [ ] HTTP client redirect execution path for relative vs absolute redirects, redirect-limit exhaustion, and sensitive header handling across host changes; builder knobs are covered, but network redirect behavior still needs a local two-server E2E.
- [ ] HTTP request body double-set debug assertion path; needs an assert-probe binary like the recv-bundle probes because release builds intentionally use last-wins behavior.

## Test coverage gap pass 4 (2026-05-14)

Implemented in this patch:

- [x] HTTP response non-text body-kind transitions: direct tests now cover SSE, WebSocket upgrade, mapped-file, streamed-file, and deferred payload setters/accessors/take helpers, plus text reset through `text_body_mut()`.
- [x] `DeferredResponse` eventfd wake-read behavior: direct tests now drain the eventfd after `complete()` and `expire_if_past_deadline()`, verify single-wake first-result semantics, and verify 504 materialization on expiry.

Still implemented but not directly covered by deterministic tests after pass 4:

- [ ] HTTP parser/helper duplicate implementation drift: `http_server_impl.cxx` still has local copies of helper logic next to the exported `conflux.net.http_server_helpers` module; tests cover the exported helper API, but not that the legacy local copies stay byte-for-byte behavior-compatible until removed.
- [ ] HTTP client redirect execution path for relative vs absolute redirects, redirect-limit exhaustion, and sensitive header handling across host changes; builder knobs are covered, but network redirect behavior still needs a local two-server E2E.
- [ ] HTTP request body double-set debug assertion path; needs an assert-probe binary like the recv-bundle probes because release builds intentionally use last-wins behavior.

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
  - [x] Static implementation split slice: export `conflux::http_response`, `conflux::http_static_core`, and `conflux::http_static_async`; move HTTP response/deferred body vocabulary, static path normalization/cache store, static root-dir lifetime, contained open/probe helpers, static GET/PUT/DELETE execution, and async file helper coroutines out of router-owned module units.
  - [x] Dependency-edge cleanup slice: stale `socket_io -> file_io`, `net.client -> file_io`, and `body_json(NodeRef)` proposal edges are gone; higher-level targets/tests/benches no longer repeat direct `PkgConfig::LIBURING` links already propagated by lower-level runtime/file/socket targets.
- [ ] Split optional components: DB and any remaining static route registration internals. TLS, HTTP/2, HTTP/3, static option/core/async targets, SSE realtime types, and WebSocket realtime types now have separate link targets.
- [ ] CI: all examples compile, fuzz smokes, JSONTestSuite, ASan/UBSan lane, bench regression budget
- [x] Docs: versioning/semver policy, security disclosure, supported compiler+kernel matrix — added `docs/project-policy.md` and linked it from README/pre-v1 contract
- [x] HTTP security corpus: smuggling, duplicate Content-Length, TE ambiguity, Host ambiguity, chunk edge cases — documented in `docs/http-security-corpus.md`; raw-wire E2E cases added for Host and CL+TE smuggling

## Do not implement

- ORM, DI container, code generator
- Default-on ring/worker pinning or `MAP_HUGETLB`
- Full P2300 rewrite before measuring root `Task<T>` cost
- `RECV_ZC` production integration (kernel behavior not stable)
