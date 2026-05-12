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
- [ ] O_TMPFILE atomic publish: both current implementations (router.cxx:1677-1702 and file_io.cxx:2938-2967) are not atomic — unlink before link loses the file on crash. Replace with staged link + rename protocol (link to staging name, fsync, rename to final path)
- [ ] Root `Task<T>`: add alloc counters for frames/control-blocks, then pool `ControlBlockModel<T>`
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
- [ ] Ring metrics: expose `sq_dropped`, `cq_overflow`, `accepted_direct_failures`, `zc_notif_pending`, recv-bundle stats, SEND_ZC usage/copy/adaptive-disable counters as observable counters
- [x] io_uring startup log: make setup flag fallback stripping exact (log which of `NO_SQARRAY`, `SUBMIT_ALL`, `CQE_MIXED` were stripped; now logs requested/active/stripped setup-flag sets)

## Packaging / release blockers

- [ ] CMake install + exported package config + namespaced targets. Foundation: `conflux::core`, `conflux::file_io_sync`, `conflux::file_map`, `conflux::runtime`, `conflux::file_io`, `conflux::socket_io`, `conflux::dns`, `conflux::crypto`, `conflux::json`, `conflux::json_file`, `conflux::template`, `conflux::template_watch`. HTTP: `conflux::http_core`, `conflux::http_router`, `conflux::http_server`, `conflux::http_static_core`, `conflux::http_static_async`, `conflux::http_auth`, `conflux::http_json`, `conflux::http_policy`, `conflux::http_observability`, `conflux::http_openapi`, `conflux::http_realtime`, `conflux::http_vhost`, `conflux::http_client_sync`, `conflux::http_client_async`
- [ ] Split optional components: DB, TLS, HTTP/2, HTTP/3 as separate link targets
- [ ] CI: all examples compile, fuzz smokes, JSONTestSuite, ASan/UBSan lane, bench regression budget
- [x] Docs: versioning/semver policy, security disclosure, supported compiler+kernel matrix — added `docs/project-policy.md` and linked it from README/pre-v1 contract
- [ ] HTTP security corpus: smuggling, duplicate Content-Length, TE ambiguity, slowloris, chunk edge cases

## Do not implement

- ORM, DI container, code generator
- Default-on ring/worker pinning or `MAP_HUGETLB`
- Full P2300 rewrite before measuring root `Task<T>` cost
- `RECV_ZC` production integration (kernel behavior not stable)
