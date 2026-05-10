# io_uring / Socket — Remaining Work

_Collapsed from `conflux_linux_first_class_io_uring_todo_proposal.md`. Completed P0/P1/P2 items removed._

---

## Bugs

### Bundle recycling in fatal-drain path (`http_server.cxx:3519-3528`)

`recycle_recv_buffer_direct` uses arithmetic `(buf_id+1)%count` to walk successive buffer IDs in bundle mode. This assumes sequential allocation, which conflicts with the non-sequential ring-order fix applied elsewhere.

- [x] Fix to use ring-order consume head: `consume(cnt)` + `recycle_range(start, cnt)`.

---

## P1 — Active

### HTTP async client (P1-02)

- [ ] HTTPS async path: currently `not implemented`.
- [ ] Proxy plaintext path: still has blocking send (`proxy.cxx:88`).
- [ ] Connect timeout with linked timeout or explicit cancel.
- [ ] Cancellation-safe close path.
- [ ] Happy Eyeballs connect staggering/racing.
- [ ] Remove/deprecate `FileReader` socket methods after HTTP/DNS migration complete.

### DNS transport cleanup (P1-03)

TCP fallback already uses `SocketTaskRing`. Remaining:

- [x] Remove `FileReader` from blocking UDP DNS path — replaced with `block_on_socket_task(tmp_str, ...)`.
- [x] Remove temp-ring allocation from per-query blocking DNS path — thread-local `TlsRingBase` + `SocketTaskRing` reused across calls.
- [x] Add thread-local reusable `SocketTaskRing`/ring for blocking compatibility path.
- [x] Add cancellation-aware linked timeout for UDP queries — already implemented via `UdpSocket::recv_from(buf, timeout)` using `submit_recvmsg_timeout_borrowed`.
- [ ] Add caller-provided `SocketTaskRing` path for async DNS.

### Direct accept TCP_NODELAY (P1-04)

`queue_direct_accept_setup` (`http_server.cxx:1794`) only sets `tcp_quickack_once`. Non-direct accepted sockets get `TCP_NODELAY` via raw `setsockopt` at line 2330; direct sockets silently skip it.

- [x] Enable `setup.tcp_nodelay_once = caps.cmd_sock_setsockopt` in `queue_direct_accept_setup`.

### Cancellation above socket layer (P1-08)

PR A (server multishot cancel + shutdown) and PR B (DNS + HTTP client timeouts) complete.
Benchmark gate passed (main vs db, release-clang-libcxx, 2026-05-10). Remaining:

- [ ] Cancel-by-fd/close-fd for multishot recv where user-data cancel is insufficient — deferred; generation check mitigates, proper fix requires per-fd cancel slot tracking.
- [ ] `HttpTimeouts::write` async path — currently ignored; add linked timeout to `do_send` write SQE.
- [ ] HTTPS async cancellation path — `client_async.cxx` TLS connect/recv not yet cancellation-aware.
- [ ] Shutdown explicit recv cancel for armed connections — follow-up after PR B merge.
- [ ] WebSocket handoff cancel — already implemented (`ws_cancel_handoffs` / `queue_ws_cancel`); no action needed.

### Benchmarks (P1-09)

- [x] `block_on_socket_task()` helper landed (`socket_io_coro.cxx`).
- [x] `tcp_accept` + `tcp_accept_multishot` async coroutine API landed (`socket_io_coro.cxx`) — P1-09a complete.
  - Tests: 9 cases in `socket_task_ring_test.cxx` (E2E, cancel, SQ-full retry, submit_on_owner fail, lifetime).
  - compare-bins gate passed 2026-05-10: `str/*` vs `fr/*` in `tcp_increment_coro_bench`,
    release-clang-libcxx — callback +1.7%, coroutine -0.5% (±2% pass).
- [x] `SocketTaskRing` vs `FileReader` client variants — `str/callback` and `str/coroutine` landed in `tcp_increment_coro_bench` (e3f1038, 2026-05-10); compare-bins gate passed. `tcp_socket_task_bench` deleted (absorbed).
- [ ] Async server variant (`str/async_callback`, `str/async_coroutine`) — requires `tcp_accept_multishot`-based server loop in `tcp_increment_coro_bench`.
- [ ] N=4 parallel clients variant (`str/parallel_4`) — requires `join_all` of N `TcpStream` coroutines on one ring.
- [ ] Close-direct deferred path benchmarks — requires `FlowRuntime` integration.

---

## P2 — Active

### Poll-first recv benchmark (P2-03)

Plumbing is complete (`RecvArmPolicy`, `resolve_recv_arm_policy`, server integration, `auto_recv_arm_policy` config knob, caps). Remaining:

- [ ] Benchmark `default_` vs `poll_first` vs adaptive under idle/bulk traffic.

### Ring resize — server auto-grow (P2-01 follow-up)

- [ ] Track `saw_overflow_since_last_resize`; call `grow_cq_to` once after overflow clears (no server auto-grow in initial implementation).

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
