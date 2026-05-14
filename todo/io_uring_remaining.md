# io_uring / Socket — Remaining Work

_Collapsed from `conflux_linux_first_class_io_uring_todo_proposal.md`. Completed P0/P1/P2 items removed._

---

## Bugs

### Bundle recycling in fatal-drain path (`http_server.cxx:3519-3528`)

`recycle_recv_buffer_direct` uses arithmetic `(buf_id+1)%count` to walk successive buffer IDs in bundle mode. This assumes sequential allocation, which conflicts with the non-sequential ring-order fix applied elsewhere.

- [x] Fix to use ring-order consume head: `consume(cnt)` + `recycle_range(start, cnt)`.

### Current debug-clang-libcxx regression

- [ ] `http.cq_overflow: non-UB shutdown under small-CQ flood` aborts in `tests/http_overflow_stress_tests` during the full `debug-clang-libcxx` preset run; the failure reproduces in `src/net/io_buffer.cxx:894`.

---

## P1 — Active

### HTTP async client (P1-02)

- [x] HTTPS async path — `TcpTlsStream` (memory BIO, async handshake/read/write/close) landed in `fbf7ffd`.
- [x] Cancellation-safe close path — `CloseState` shielded close landed in `fbf7ffd`.
- [x] Connect timeout with linked timeout or explicit cancel — linked SQE + staggered connect in `fbf7ffd`.
- [x] Happy Eyeballs connect staggering/racing — `HappyConnectState`/`staggered_parallel_connect` (RFC 8305 v1, 250 ms stagger) landed in `fbf7ffd`.
- [x] `HttpTimeouts::write` async path — `submit_send_timeout_borrowed` (linked SQE + timeout) landed in `fbf7ffd`.
- [x] Proxy async migration — `proxy_context_handler()` / `ContextHandler` / `dispatch_async()` landed in `844b8dc`. `proxy_handler()` kept as deprecated `work_pool` fallback.
- [x] Remove/deprecate `FileReader` socket methods after HTTP/DNS migration complete. The stale `socket_io` → `file_io` CMake PUBLIC link has been removed; remaining `FileReader` socket users are explicit compatibility/benchmark paths, and the methods are now deprecated in `FileReader` itself.
- [x] `FileReader::atomic_write_async()` now mirrors the sync staged publish protocol: validate/split contained path, stage in final parent dir, O_TMPFILE with named-temp fallback, link staging + rename/renameat2, cleanup staging on failure, fsync final parent dir.
- [x] HTTPS async cancellation — `client_async.cxx` now threads `ActiveTaskCancelRelay` through connect, TLS handshake, and TLS recv/write paths.

### DNS transport cleanup (P1-03)

TCP fallback already uses `SocketTaskRing`. Remaining:

- [x] Remove `FileReader` from blocking UDP DNS path — replaced with `block_on_socket_task(tmp_str, ...)`.
- [x] Remove temp-ring allocation from per-query blocking DNS path — thread-local `TlsRingBase` + `SocketTaskRing` reused across calls.
- [x] Add thread-local reusable `SocketTaskRing`/ring for blocking compatibility path.
- [x] Add cancellation-aware linked timeout for UDP queries — already implemented via `UdpSocket::recv_from(buf, timeout)` using `submit_recvmsg_timeout_borrowed`.
- [x] Add caller-provided `SocketTaskRing` path for async DNS.

### Direct accept TCP_NODELAY (P1-04)

`queue_direct_accept_setup` (`http_server.cxx:1794`) only sets `tcp_quickack_once`. Non-direct accepted sockets get `TCP_NODELAY` via raw `setsockopt` at line 2330; direct sockets silently skip it.

- [x] Enable `setup.tcp_nodelay_once = caps.cmd_sock_setsockopt` in `queue_direct_accept_setup`.

### Cancellation above socket layer (P1-08)

PR A (server multishot cancel + shutdown) and PR B (DNS + HTTP client timeouts) complete.
Benchmark gate passed (main vs db, release-clang-libcxx, 2026-05-10). Remaining:

- [x] Cancel-by-fd/close-fd for multishot recv where user-data cancel is insufficient — the server now tears down armed multishot recv connections via fd-cancel on shutdown, and the shutdown e2e covers the no-stall close path.
- [x] `HttpTimeouts::write` async path — `submit_send_timeout_borrowed` landed in `fbf7ffd`.
- [x] HTTPS async cancellation path — `client_async.cxx` TLS connect/recv is cancellation-aware via `ActiveTaskCancelRelay` and `TcpTlsStream`.
- [x] Shutdown explicit recv cancel for armed connections — `handle_shutdown()` now cancels armed recv operations before queuing close, including deferred direct/socket handles.
- [x] WebSocket handoff cancel — already implemented (`ws_cancel_handoffs` / `queue_ws_cancel`).

### Benchmarks (P1-09)

- [x] `block_on_socket_task()` helper landed (`socket_io_coro.cxx`).
- [x] `tcp_accept` + `tcp_accept_multishot` async coroutine API landed (`socket_io_coro.cxx`) — P1-09a complete.
  - Tests: 9 cases in `socket_task_ring_test.cxx` (E2E, cancel, SQ-full retry, submit_on_owner fail, lifetime).
  - compare-bins gate passed 2026-05-10: `str/*` vs `fr/*` in `tcp_increment_coro_bench`,
    release-clang-libcxx — callback +1.7%, coroutine -0.5% (±2% pass).
- [x] `SocketTaskRing` vs `FileReader` client variants — `str/callback` and `str/coroutine` landed in `tcp_increment_coro_bench` (e3f1038, 2026-05-10); compare-bins gate passed. `tcp_socket_task_bench` deleted (absorbed).
- [x] Async server variant (`str/async_callback`, `str/async_coroutine`) — multishot-accept server + async client variants landed in `9851640`.
- [x] N=4 parallel clients variant (`str/parallel_4`) — landed in `9851640`; `--clients`/`--config` args + parallel_4 config.
- [x] Close-direct deferred path benchmarks — `socket_raw_bench` now forces a deferred close and measures `abandon_deferred_closes()` on the shutdown cleanup path.

---

## P2 — Active

### Poll-first recv benchmark (P2-03)

Plumbing is complete (`RecvArmPolicy`, `resolve_recv_arm_policy`, server integration, `auto_recv_arm_policy` config knob, caps). Remaining:

- [x] Benchmark `default_` vs `poll_first` vs adaptive under idle/bulk traffic — added `socket_raw_bench` policy-trace variants for idle/bulk traces so the arm-policy modes are exercised in the benchmark suite.

### Ring resize — server auto-grow (P2-01 follow-up)

- [x] Track `saw_overflow_since_last_resize`; call `grow_cq_to` once after overflow clears (server now attempts CQ growth after NODROP overflow drains; unsupported kernels stay on the drain fallback).

---

## P3 — Deferred

- **Registered buffer cloning (P3-01):** add wrapper only when a multi-ring fixed-buffer consumer exists; benchmark startup cost first.
- **ZC recv (P3-02):** separate design doc required; prototype only after direct-slot/buffer/CQ ownership model is stable. Do not mix into current `BufferRing` API.
- **AF_ALG (P3-03):** benchmark-gated; add only after benchmark proves value over AESNI for target payload sizes.

### DNS v2 (deferred from DNS proposal)

- inotify watch on `resolv.conf` — v2.
- DoH/DoT — backend stub reserved; not implemented.
- SRV/MX — separate `resolve_mx`/`resolve_srv` API; v2.
- IDNA punycode — caller responsibility; v2 helper.

---

## Warning

Do not claim perf wins without same-hardware measurements under realistic HTTP load. ZC recv, registered network send buffers, IOPOLL, busy-poll, and lock-free worker queues must be separate measured branches.
