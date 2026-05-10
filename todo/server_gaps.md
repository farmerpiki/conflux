# Server / Framework Gaps

## Immediate

- [x] Direct-accept: set `tcp_nodelay_once` in `http_server.cxx` `DirectTcpAcceptSetup` (normal-accept already sets it)
- [x] JSON docs: sync `docs/json-api.md` field names with current `JsonError` / error-code names
- [x] JSON arena: `JsonArena::parse_borrowed_into(string_view)` and `parse_moved_into(string&&)` landed
- [ ] JSON hash index: allocate via PMR resource, not global `::operator new`
- [ ] Ring thread affinity: add `ring_core` / `worker_core_base` config fields, default disabled
- [ ] Busy poll: add `busy_poll_us` / `prefer_busy_poll` config fields, default disabled

## Medium

- [ ] HTTP send path: registered send-buffer pool for headers/small bodies
- [ ] HTTP send path: `SEND_ZC` integration with notification-CQE tracking, capability-gated
- [ ] Root `Task<T>`: add alloc counters for frames/control-blocks, then pool `ControlBlockModel<T>`
- [ ] JSON: true incremental `JsonStreamReader::feed(span<byte>)` / events API
- [ ] Router: `HandlerResult` / `Middleware` concepts with readable static-assert messages
- [ ] io_uring init: adaptive flag fallback on `EINVAL` (strip `CQE_MIXED` → `NO_SQARRAY` → `TASKRUN_FLAG` → `DEFER_TASKRUN` → `SINGLE_ISSUER`, log final set)

## Architecture (dedicated branches)

- [ ] Dedicated `IOPOLL` ring for O_DIRECT file I/O, separate from network ring
- [ ] Local worker queues: profile mutex contention first, then Chase-Lev if warranted
- [ ] `admission_mtx_` in `work.cxx`: profile under high-RPS, replace with atomic if bottleneck
- [ ] Ring metrics: expose `sq_dropped`, `cq_overflow`, `accepted_direct_failures`, `zc_notif_pending` as observable counters

## Packaging / release blockers

- [ ] CMake install + exported package config + namespaced targets (`conflux::http`, `conflux::json`, etc.)
- [ ] Split optional components: DB, TLS, HTTP/2, HTTP/3 as separate link targets
- [ ] CI: all examples compile, fuzz smokes, JSONTestSuite, ASan/UBSan lane, bench regression budget
- [ ] Docs: versioning/semver policy, security disclosure, supported compiler+kernel matrix
- [ ] HTTP security corpus: smuggling, duplicate Content-Length, TE ambiguity, slowloris, chunk edge cases

## Do not implement

- ORM, DI container, code generator
- Default-on ring/worker pinning or `MAP_HUGETLB`
- Full P2300 rewrite before measuring root `Task<T>` cost
- `RECV_ZC` production integration (kernel behavior not stable)
