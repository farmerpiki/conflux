# Conflux Linux/io_uring First-Class Library TODO Proposal

Repository snapshot inspected: uploaded `conflux-db.zip`, plus `io_uring_direct_file_flow_design.md` and `SOCKET_IO_PROPOSAL.md`.

## Bottom line

Current state is already strong for **raw HTTP server io_uring primitives**: raw `socket_io` exists, server migration to raw helpers is largely done, direct-file flow exists with a substantial test suite, and transport benchmarks exist. It is not yet “first-class Linux library” quality because the ownership and lifecycle contracts are still split across `socket_io`, `file_io`, `http_server`, and `uring.flow`.

The highest-risk gaps are not missing SQE prep helpers. They are:

- buffer ownership under recv bundle / future incremental provided buffers;
- hidden submit behavior in raw APIs;
- zero-copy-send completion semantics;
- direct-slot lease and poisoned-slot state;
- CQ overflow policy for managed flows;
- coroutine socket APIs still depending on `FileReader`;
- scattered kernel feature gating.

## Classification legend

- **Keep** — implement as proposed.
- **Keep, narrow** — idea is valid, but scope/order should change.
- **Defer** — track, but do not spend implementation time until prerequisites or benchmarks justify it.
- **Drop from active TODO** — not a current implementation task.
- **Partial/unsafe** — code exists, but should not be considered done.

## Priority classification table

| ID | Proposed item | Classification | Priority | Current-state judgment |
|---:|---|---|---|---|
| 1 | Recv bundle decoding | **Keep, narrow** | P0 | Partial/unsafe. Current server has ad-hoc multi-buffer copy, but `BufferRing` has no first-class bundle state/API/tests. |
| 2 | `SocketRawRing::get_sqe()` hidden submit | **Keep** | P0 | Correctness/ergonomics blocker for raw batching. |
| 3 | ZC send notification semantics | **Keep, narrow** | P0 | Public async API is a footgun; do not promote to `socket_io` until notification CQE is modeled. |
| 4 | Direct-slot lease/poison model | **Keep** | P0 | Needed to make direct fds and direct-file flows coherent. |
| 5 | CQ overflow policy | **Keep** | P0 | Required before managed-flow guarantees are trustworthy under load. |
| 6 | Real `SocketTaskRing` | **Keep** | P1 | Coroutine socket layer currently wraps `FileReader`; this blocks clean socket API. |
| 7 | Async HTTP client migration | **Keep, narrow** | P1 | Do after #6; current async client still rides `FileReader` and lacks full connect policy. |
| 8 | DNS reusable async transport | **Keep** | P1 | Temp ring + FileReader transport still present. |
| 9 | `TcpListener` abstraction | **Keep** | P1 | Ergonomic surface missing even though raw helpers exist. |
| 10 | Buffer-ring modes: classic/bundle/incremental | **Keep** | P1 | Should absorb #1 and future incremental mode; avoid one misleading `buffer_view(id,len)` API. |
| 11 | Registered buffer cloning | **Defer** | P3 | Useful capability, but not urgent for current provided-buffer server path. Track only. |
| 12 | Ring resize hooks | **Keep, narrow** | P2 | Add after CQ telemetry; useful but has kernel/setup constraints. |
| 13 | io_uring ZC Rx | **Defer** | P3 | Research/design tracker only; ownership model differs too much from provided buffers. |
| 14 | Poll-first/send policy | **Keep** | P3 | Small performance policy layer after correctness work. |
| 15 | Runtime capability matrix | **Keep** | P1 | Promote earlier; it gates #1/#10/#12/#13 cleanly. |
| 16 | Lifetime-contract API split | **Keep** | P1 | Needed before Task APIs are ergonomic and safe. |
| 17 | Cancellation semantics | **Keep** | P1 | Start with connect/DNS/HTTP timeout paths. |
| 18 | Owned-path flow variants | **Keep** | P2 | Enables SQPOLL/no-submit-stable usability without unsafe borrowed paths. |
| 19 | Benchmark gates | **Keep, narrow** | P1 | Some benches already exist; extend gates to new ownership and feature paths. |

---

# P0 — correctness / footgun blockers

## [x] P0-01: Make recv bundle either correct or unavailable

**Classification:** Keep, narrow.  
**Current state:** Done — `a2784c0`, `7aef873`, `7a539d8`, `6ead3e1`, `c40a37e`.

**TODO:**

- [x] Add `BufferRingMode::classic_one_cqe_per_buffer`.
- [x] Add `BufferRingMode::recv_bundle`.
- [x] Track cached consume head for bundle rings.
- [x] Add `RecvBundle` decode API that returns all chunks for a CQE.
- [x] Make recycling bundle-aware; recycle exactly all consumed buffers, once.
- [x] Add tests for bundle crossing buffer boundaries, wraparound, short first buffer, and multishot end.
- [x] Until tests pass, force `recv_bundle=false` even if kernel advertises the feature.

Suggested API:

```cpp
struct RecvChunk {
    u16 id;
    u32 offset;
    span<byte const> bytes;
};

struct RecvBundleView {
    span<RecvChunk const> chunks;
    u32 total;
    bool multishot_more;
};

expected<RecvBundleView, Err> BufferRing::decode_bundle_cqe(int res, u32 flags) noexcept;
void BufferRing::recycle_bundle(RecvBundleView const&) noexcept;
```

Implementation note: avoid heap allocation in the hot path. Store chunks in caller-provided fixed storage or expose an iterator/callback:

```cpp
template<class Fn>
expected<u32, Err> BufferRing::for_each_bundle_chunk(int res, u32 flags, Fn&& fn) noexcept;
```

## [x] P0-02: Remove hidden submit from `SocketRawRing::get_sqe()`

**Classification:** Keep.  
**Current state:** Done — `a2784c0`.

**TODO:**

- [x] Replace `get_sqe()` with `try_get_sqe() noexcept` that never submits.
- [x] Add `reserve_sqe_slots(u32 n) noexcept`.
- [x] Add `get_reserved_sqe() noexcept` for after reservation.
- [x] Update every raw helper to return `false`/`Err::sq_full` instead of submitting.
- [x] Audit helpers that need multiple SQEs (`submit_shutdown_close`, `submit_recvmsg_with_timeout`) for all-or-nothing reservation.
- [x] Keep `submit()` explicit on `SocketRawRing`.

Suggested API:

```cpp
class SocketRawRing {
public:
    [[nodiscard]] io_uring_sqe* try_get_sqe() noexcept;
    [[nodiscard]] bool reserve_sqe_slots(u32 n) noexcept;
    [[nodiscard]] io_uring_sqe* get_reserved_sqe() noexcept;
    int submit() noexcept;
};
```

## [x] P0-03: Make zero-copy send notification-aware or mark it experimental/unsafe

**Classification:** Keep, narrow.  
**Current state:** Done — `800a004`.

Evidence in current repo:

- `src/file_io/file_io.cxx:2612-2615` says `send_zc_async()` resolves on the first CQE and discards the notification CQE.
- `src/file_io/file_io.cxx:2929-2932` says `sendmsg_zc_async()` also resolves on first CQE while the notification remains separate.

For a coroutine API, “task completed” should not imply “the user may free/reuse the buffer” unless the notification CQE has arrived. Current behavior is acceptable only for advanced raw users who explicitly track the notification lifetime.

**TODO:**

- [x] Do not add current `send_zc_async()` shape to `socket_io` Task API.
- [x] Rename existing first-CQE behavior to an explicit advanced form, e.g. `send_zc_submit_only_borrowed()`.
- [x] Add raw ZC API returning a token that owns/identifies the notification CQE.
- [x] Add safe Task API that completes only after the notification CQE.
- [x] Add tests that mutate/free the buffer after first CQE but before notification in debug/fault mode.

Suggested API split:

```cpp
struct SendZcResult {
    SZ submitted_bytes;
    bool used_zero_copy;
};

Task<SendZcResult> send_zc(span<byte const> data);       // completes after notification
Task<SZ> send_zc_submit_only_borrowed(span<byte const>);  // explicit advanced API
RawZcToken submit_send_zc_borrowed(...);                  // caller dispatches notification
```

## [x] P0-04: Add `DirectSlotPool` with poison state

**Classification:** Keep.  
**Current state:** Done — `05d41b9`, `4d85d74`, `e101826`, `312bca2`, `9fcd9a4` (leased_empty/acquire/release_empty/DirectSlotLease added).

**TODO:**

- [x] Introduce `DirectSlotPool` independent of `DirectFdTable`.
- [x] Model states: `free`, `leased_empty`, `populated`, `closing`, `poisoned`.
- [x] Return RAII `DirectSlotLease` for acquired slots.
- [x] Support “kernel allocated direct slot” adoption from `accept_direct` / `socket_direct_alloc`.
- [x] Mark slot poisoned on `close_direct` failure.
- [x] Ensure poisoned slots never re-enter generic free pool.
- [x] Wire into `uring.flow`, `socket_io`, WebSocket fixed-fd handoff, and direct-close paths.

Suggested API:

```cpp
enum class DirectSlotState : u8 {
    free,
    leased_empty,
    populated,
    closing,
    poisoned,
};

class DirectSlotPool {
public:
    expected<DirectSlotLease, Err> acquire() noexcept;
    expected<void, Err> adopt_kernel_allocated(DirectSlot slot) noexcept;
    void mark_populated(DirectSlot) noexcept;
    void release_empty(DirectSlot) noexcept;
    void release_closed(DirectSlot) noexcept;
    void poison(DirectSlot, int close_res) noexcept;
};
```

## [x] P0-05: Add CQ overflow accounting and fatal/quarantine policy

**Classification:** Keep.  
**Current state:** Done — `c1ea3a5` (policy + flush + RunStatus + stress tests), `66a4687` (FlowRuntime shutdown-abandon hook).

Fatal-on-overflow policy implemented with NODROP branching, bounded flush, `RunStatus` propagation, recv-buf recycling in flush, and ring-level stress tests. `FlowRuntime::abandon_deferred_closes()` handles shutdown drain.

Bug fix (`9fcd9a4`): `http_server.cxx` was incorrectly calling `enter_ring_fatal` + returning when `FEAT_NODROP` was set and overflow was detected. With NODROP, CQEs are buffered in the overflow list — not lost — and `io_uring_submit_and_wait` drains them on the next call. Fixed: NODROP overflow is no longer treated as a fatal condition; the run loop continues.

**TODO:**

- [x] Add ring-level `cq_has_overflow()` wrapper.
- [x] Add `cq_overflow_count()` / stats from ring overflow counters where available.
- [x] Call `io_uring_get_events()` when overflow entries are waiting and feature support exists.
- [x] Define policy: managed-flow CQE lost means fatal ring error or ring quarantine.
- [x] Export overflow metrics through server diagnostics.
- [x] Add stress tests that intentionally under-size CQ and verify policy.
- [x] Add shutdown path that drains/abandons pending direct-file-flow deferred closes before ring teardown.

---

# P1 — async ergonomics and ownership model

## [ ] P1-01: Implement real `SocketTaskRing`

**Classification:** Keep.  
**Current state:** Not done. Worktree `p1-socket-task-ring` at `~/conflux_dev/p1_socket_task_ring`; proposal `p1_01_socket_task_ring_proposal.md` in `~/conflux_dev/`.

Evidence in current repo:

- `src/socket_io/socket_io_coro.cxx:22-24` says `TcpStream` wraps `FileReader`.
- `src/socket_io/socket_io_coro.cxx:26-59` stores `FileReader*` in `TcpStream` and calls FileReader socket methods.
- `src/socket_io/socket_io_coro.cxx:82-114` stores `FileReader*` in `UdpSocket`.
- `src/socket_io/socket_io_coro.cxx:115-141` uses synchronous `::socket()` and `::bind()` for UDP ephemeral creation.

The original socket plan called for raw `SocketRawRing` plus future `SocketTaskRing`; the current coroutine layer is a compatibility shim, not the final socket abstraction.

**TODO:**

- [ ] Add `SocketTaskRing` wrapping `SocketRawRing`, `CompletionTable`, and user-data encoder.
- [ ] Move `TcpStream` to `SocketTaskRing* + SocketHandle`.
- [ ] Move `UdpSocket` to `SocketTaskRing* + SocketHandle`.
- [ ] Reimplement `tcp_connect` through socket raw helpers and completion slots.
- [ ] Reimplement UDP send/recv through socket raw helpers.
- [ ] Preserve `_borrowed` naming for raw APIs; use safe Task names for owned/copying APIs.
- [ ] Add tests proving no `FileReader` dependency remains in `conflux.socket_io.coro`.

Suggested shape:

```cpp
class SocketTaskRing {
    SocketRawRing raw_;
    CompletionTable* completions_{};
    UserDataFn encode_{};
public:
    [[nodiscard]] SocketRawRing& raw() noexcept;
    [[nodiscard]] CompletionTable& completions() noexcept;
    [[nodiscard]] u64 encode(u32 slot, u32 gen) noexcept;
};

class TcpStream {
    SocketTaskRing* ring_{};
    SocketHandle handle_{};
};
```

## [ ] P1-02: Migrate async HTTP client after `SocketTaskRing`

**Classification:** Keep, narrow.  
**Current state:** Partial.

Evidence in current repo:

- `src/net/client_async.cxx` exists and implements plaintext async behavior.
- It imports `conflux.socket_io.coro`, but that layer still wraps `FileReader`.
- `src/net/client_async.cxx:193-206` tries endpoints sequentially without connect-attempt staggering/racing.
- `src/net/proxy.cxx:2` and `src/net/proxy.cxx:88` still use `send_blocking`.

Do not migrate more HTTP surface onto the current `FileReader`-backed coroutine socket API. First land `SocketTaskRing`, then move the async HTTP client/proxy to it.

**TODO:**

- [ ] Use `SocketTaskRing` for async plaintext HTTP client.
- [ ] Add connect timeout with linked timeout or explicit cancel.
- [ ] Add cancellation-safe close path.
- [ ] Add Happy Eyeballs connect staggering/racing.
- [ ] Apply socket options through a clear policy at socket creation/accept where possible.
- [ ] Migrate proxy plaintext path to async.
- [ ] Keep TLS path blocking until async TLS exists.

## [ ] P1-03: Replace DNS temp-ring/FileReader transport with reusable socket transport

**Classification:** Keep.  
**Current state:** Partial.

Evidence in current repo:

- `src/net/dns/dns.cxx:1810-1830` still creates a temporary io_uring ring and `FileReader` for blocking native UDP resolution.
- `src/net/dns/dns.cxx:959` uses `tcp_connect(reader, ...)`, i.e. still via FileReader-backed `TcpStream`.

**TODO:**

- [ ] Add caller-provided `SocketTaskRing` path for async DNS.
- [ ] Add thread-local reusable `SocketTaskRing`/ring for blocking compatibility path.
- [ ] Move UDP send/recv timeout to `UdpSocket` backed by `socket_io`, not `FileReader`.
- [ ] Add cancellation-aware linked timeout for UDP queries.
- [ ] Ensure TCP DNS fallback uses `SocketTaskRing`.
- [ ] Remove temp-ring allocation from per-query blocking DNS path.

## [x] P1-04: Add `TcpListener` as a real high-level abstraction

**Classification:** Keep.  
**Current state:** Phase 1 done (branch `p1-tcp-listener`, commits `5732b21`, `81ccee5`).

**Phase 1 — done:**

- [x] `TcpBindAddress` enum (`loopback_v4`, `any_v4`, `loopback_v6`, `any_v6_dual`, `any_v6_only`).
- [x] `TcpListenerOptions` struct: `port`, `bind`, `reuse_addr`, `reuse_port` (opt-in, default false), `backlog`, `accept_flags`.
- [x] `TcpListener` RAII class: socket/setsockopt/bind/listen/getsockname in ctor; move-only; throws `std::system_error` on any failure.
- [x] `arm_accept_multishot_borrowed` / `rearm_accept_multishot_borrowed` forwarding `IoUringCaps` and `accept_flags_`.
- [x] `submit_accept_multishot_borrowed`: new overloads with explicit `accept_flags` param; old zero-flags signatures kept as `noexcept` compat wrappers.
- [x] `DirectTcpAcceptSetup`: `tcp_nodelay_once{false}`, `tcp_quickack_once{false}`, `skip_sockopt_success_cqes{true}`; TCP_NODELAY SQE emitted when enabled.
- [x] Tests: ephemeral port, loopback_v4/v6 getsockname, arm v4/v6 accept CQE, accept_flags forwarding (O_NONBLOCK/FD_CLOEXEC), reuse_port second bind, deterministic rearm via cancel.

**Phase 2 — deferred** (needs `DirectFdTable::clear(slot)` first):

- [ ] `install_direct(DirectFdTable&, u32 slot)` — install listen fd into a registered-files slot.

**Phase 3 — deferred** (user migration):

- [ ] Migrate `benchmarks/socket_raw_bench.cxx::make_listen_socket()` to `TcpListener`.
- [ ] Migrate `src/net/http_server.cxx` accept setup (after Phase 2 and direct-slot bookkeeping are solid).

**Separate fix (implemented, callers opt-in):**

- [x] `DirectTcpAcceptSetup::tcp_nodelay_once` — TCP_NODELAY on direct accepted sockets; previously missing from the direct-accept path.

## [ ] P1-05: Add explicit buffer-ring modes

**Classification:** Keep.  
**Current state:** Partial — `BufferRingMode` enum scaffolded (`classic_one_cqe_per_buffer`, `recv_bundle`, `incremental`); `incremental` rejected at ctor with precise error (`9fcd9a4`). Worktree `p1-incremental-buf` at `~/conflux_dev/p1_incremental_buf`; proposal `p1_05_incremental_buf_ring_proposal.md` in `~/conflux_dev/`.

**TODO:**

- [x] Add `BufferRingMode` to `BufferRingOptions`.
- [x] Keep classic mode as the default (`classic_one_cqe_per_buffer`).
- [x] Add bundle mode with cached head tracking.
- [ ] Add incremental mode with per-buffer offset tracking and `IORING_CQE_F_BUF_MORE` handling.
- [x] Reject unsupported modes at construction with precise error.
- [x] Add mode-specific tests and benchmarks.

Suggested API:

```cpp
enum class BufferRingMode : u8 {
    classic_one_cqe_per_buffer,
    recv_bundle,
    incremental,
};

struct BufferRingOptions {
    u32 count{4096};
    SZ buf_size{8192};
    u16 group_id{0};
    bool huge_pages{true};
    BufferRingMode mode{BufferRingMode::classic_one_cqe_per_buffer};
};
```

## [x] P1-06: Add runtime `IoUringCaps` matrix

**Classification:** Keep.  
**Current state:** Done — `73c3691`.

Evidence in current repo:

- Feature checks are scattered: HTTP server checks `IORING_FEAT_RECVSEND_BUNDLE`; flow checks `IORING_FEAT_SUBMIT_STABLE`; low-level `Ring` exposes only `has_feature(u32)`.

A first-class library should expose one capability object and force each high-level API to choose: use feature, fall back, or reject.

**TODO:**

- [x] Add `IoUringCaps` construction from `io_uring_params`, kernel/liburing headers, and runtime probes where needed.
- [x] Include feature availability and setup constraints.
- [x] Pass caps into `socket_io`, `uring.flow`, server, DNS, and client setup.
- [x] Use caps to reject unsupported buffer modes and direct-flow modes.
- [x] Add debug dump of caps in server startup logs.

Suggested shape:

```cpp
struct IoUringCaps {
    bool submit_stable{};
    bool recvsend_bundle{};
    bool pbuf_ring_inc{};
    bool multishot_accept_direct{};
    bool socket_direct_alloc{};
    bool cmd_sock_setsockopt{};
    bool resize_rings{};
    bool registered_buffer_clone{};
    bool zc_rx{};
};
```

## [ ] P1-07: Make lifetime contracts consistent across raw and Task APIs

**Classification:** Keep.  
**Current state:** Partial. Raw layer `_borrowed` renames done (`9fcd9a4`): `submit_accept_multishot_borrowed`, `submit_setsockopt_borrowed`, `submit_timeout_borrowed`, `submit_link_timeout_borrowed`. Task APIs and any remaining raw helpers not yet audited.

**TODO:**

- [x] Ensure all raw APIs with caller-owned memory use `_borrowed` suffix. *(raw layer done; Task layer pending)*
- [ ] For Task APIs, prefer owned/copying semantics unless the function name says borrowed.
- [ ] Add `send_owned(...)` or `send_all_owned(...)` where ergonomics matter.
- [ ] Add `send_static(...)` only if useful for literal/static buffers.
- [ ] Document buffer lifetime through CQE for raw APIs and through task completion for safe Task APIs.
- [ ] For ZC send, define task completion as notification completion, not first CQE.

## [ ] P1-08: Add first-class cancellation policy

**Classification:** Keep.  
**Current state:** Partial. `CancelPolicy` enum scaffolded (`ignore`, `cancel_sqe_by_user_data`, `cancel_fd`, `close_fd`) in `socket_io.cxx` (`9fcd9a4`). Task-level wiring not yet done.

**TODO:**

- [x] Define cancellation policy enum.
- [ ] Support cancel-by-user-data for connect/recv/send where safe.
- [ ] Support cancel-by-fd/close-fd where user-data cancel is insufficient.
- [ ] Apply to connect timeout, DNS timeout, HTTP request timeout, shutdown, and WebSocket handoff.
- [ ] Define result normalization for timeout vs user cancel vs fd close.
- [ ] Add tests for cancellation racing with successful CQE.

Suggested shape:

```cpp
enum class CancelPolicy : u8 {
    ignore,
    cancel_sqe_by_user_data,
    cancel_fd,
    close_fd,
};
```

## [ ] P1-09: Extend benchmark gates around ownership and feature paths

**Classification:** Keep, narrow.  
**Current state:** Partial.

Evidence in current repo:

- `benchmarks/http_server_bench.cxx`, `benchmarks/http_server_concurrency_bench.cxx`, and `benchmarks/socket_raw_bench.cxx` already exist.
- `scripts/bench_record.sh --compare-bins` and `bench_compare_summary` already exist.

Do not create duplicate benchmark infrastructure. Extend the existing gate with targeted cases for the new risk areas.

**TODO:**

- [ ] Add raw helper overhead vs old inline server SQE prep comparison.
- [x] Add recv bundle decode benchmark (`run_buf_slices_from_cqe_classic` in `socket_raw_bench.cxx`, `9fcd9a4`).
- [ ] Add incremental buffer mode benchmark.
- [ ] Add `SocketTaskRing` vs current FileReader-backed coroutine wrapper benchmark.
- [x] Add direct slot pool acquire/release/poison benchmark (`run_direct_slot_pool_acquire_release`, `run_direct_slot_pool_full_lifecycle` in `socket_raw_bench.cxx`, `9fcd9a4`).
- [ ] Add close-direct under SQ-full/deferred-cleanup benchmark.
- [ ] Require `--compare-bins` on server migration steps.

---

# P2 — capability and compatibility polish

## [ ] P2-01: Add ring resize wrapper after CQ telemetry

**Classification:** Keep, narrow.  
**Current state:** Not done. Worktree `p2-ring-resize` at `~/conflux_dev/p2_ring_resize`; proposal `p2_01_ring_resize_proposal.md` in `~/conflux_dev/`.

Ring resizing is useful for network CQ sizing, but it is not a substitute for overflow policy. Only `IORING_SETUP_DEFER_TASKRUN` rings are supported; `NO_MMAP` rings are not. `IORING_SETUP_CQSIZE` must be set in the params flags or the kernel ignores `cq_entries`. No server auto-grow in this PR — wrapper + tests only.

**TODO:**

- [ ] CMake C++ link probe (`CONFLUX_HAVE_IO_URING_RESIZE_RINGS`) — prove symbol present and linkable; propagate as `PUBLIC` compile definition on `conflux_uring`; export `build_has_io_uring_resize_rings` constexpr from module.
- [ ] `RingSize` struct — export alongside `IoUringCaps`.
- [ ] `RingRef::sq_entries()` / `RingRef::cq_entries()` accessors.
- [ ] `RingRef::resize(RingSize)` — check `!cq_has_overflow()`, `io_uring_sq_ready() == 0`, `DEFER_TASKRUN` flag, `!NO_MMAP`; set `IORING_SETUP_CQSIZE` in params; call `io_uring_resize_rings`; return `unexpected{-ENOSYS}` when link probe absent.
- [ ] `RingRef::grow_cq_to(u32)` — read `cq_entries()`, no-op if already large enough, delegate to `resize({current_sq, entries})`.
- [ ] `Ring::resize` / `Ring::grow_cq_to` — delegate to `ref()`.
- [ ] Refuse resizing while CQ overflow is active (`-EBUSY`).
- [ ] Tests — probe cap; grow small ring; verify overflow blocks resize; verify no-op path in `grow_cq_to`.
- [ ] No server auto-grow in this PR. Future: track `saw_overflow_since_last_resize` locally and call `grow_cq_to` once after overflow clears.

## [ ] P2-02: Add owned-path variants for direct-file flow

**Classification:** Keep.  
**Current state:** Not done. Worktree `p2-owned-path-flow` at `~/conflux_dev/p2_owned_path_flow`; proposal `p2_02_owned_path_flow_proposal.md` in `~/conflux_dev/`.

`FlowBuilder::submit()` rejects all flows when `!path_lifetime_stable_` (SQPOLL or no `SUBMIT_STABLE`). Owned-path flows are immune — the path bytes are copied into runtime storage keyed by slab index before the SQE is prepared, not into the temporary builder. Storing path in the builder alone is unsafe: the builder array is reused on the next `rt.flow()` call, possibly before SQPOLL consumes the SQE.

**TODO:**

- [ ] `OwnedInlinePath` struct (cap = 255, `NAME_MAX`; reject `> cap` with `-ENAMETOOLONG`; reject embedded NUL with `-EINVAL`).
- [ ] `A<OwnedInlinePath, kMaxFlows> owned_paths_{}` in `FlowRuntime` (1 MiB static; cold allocation path).
- [ ] `owns_path` flag + `owned_path_buf` staging in `DirectFileBuilder` (256 bytes staging per builder; `kMaxBatch=64` → 16 KB).
- [ ] `FlowBuilder::open_direct_owned(OwnedInlinePath)` and `open_direct_owned(string_view)` overload (sets `b.err` on path error).
- [ ] `with_direct_file_owned` template wrapper.
- [ ] `submit()` pre-pass: replace blanket `!path_lifetime_stable_` rejection with per-builder check; guard `b.err == 0` to preserve earlier path errors.
- [ ] `submit()` copy: after slab allocation, copy staging bytes into `rt_.owned_paths_[state.flow_index]`; pass override pointer into `prep_op()`.
- [ ] `prep_op()` — add `open_path_override` param; use override for open_direct ops when non-null.
- [ ] Tests — `OwnedInlinePath` limits; mixed borrowed+owned submit under `!path_lifetime_stable_`; only borrowed flows rejected.

## [ ] P2-03: Add poll-first recv/send policy wrapper

**Classification:** Keep.  
**Current state:** `RecvArmPolicy` enum added (`default_`, `poll_first`); wired into `submit_recv_multishot` via `ioprio` (`9fcd9a4`). Adaptive arm not yet implemented; benchmarks not yet run. Worktree `p2-poll-first-auto` at `~/conflux_dev/p2_poll_first_auto`; proposal `p2_03_poll_first_auto_proposal.md` in `~/conflux_dev/`.

Do **not** add `auto_from_last_cqe` to `RecvArmPolicy` — a third value silently passed through to `submit_recv_multishot` would be treated as `default_`. Resolution stays in a free function before the call.

**TODO:**

- [x] Add `RecvArmPolicy` to `submit_recv_multishot` (`default_`, `poll_first` only — keep two values).
- [ ] `IoUringCaps::recv_poll_first` — independent of `feat_recvsend_bundle` (available since 5.19, below kernel floor; set to `true` unconditionally or via kernel-floor assumption).
- [ ] `Conn::last_recv_cqe_flags` + `Conn::have_last_recv_cqe_flags` — distinguish first-recv-after-accept from no-data last recv; reset both on accept and in `conn_erase()`.
- [ ] Flag capture — centralized in `handle_recv_cqe()` after gen check and `res > 0`, before buffer ops; do not scatter next to each `queue_multishot_recv()` call.
- [ ] `resolve_recv_arm_policy(bool auto_enabled, bool recv_poll_first, bool have_last_flags, u32 last_flags)` — free function exported from `conflux.socket_io`; thin member wrapper on `Ring`.
- [ ] `queue_multishot_recv` — call `resolve_recv_arm_policy(conn)` and pass result to `submit_recv_multishot`.
- [ ] Config knob `auto_recv_arm_policy` — default `false`; wire into `[io_uring]` config, `kBoolKeys`, `flags_str`, startup log.
- [ ] `recv_poll_first` in `caps_to_log_string()`; update `caps_test.cxx`.
- [ ] Tests — free function unit tests (no-flags, cap-off, nonempty-set, nonempty-clear); benchmark `default_` vs `poll_first` vs adaptive under idle/bulk traffic.

---

# P3 — backlog / research / opt-in performance tiers

## [ ] P3-01: Track registered buffer cloning, but do not implement now

**Classification:** Defer.  
**Current state:** Not relevant to current provided-buffer server path.

Registered buffer cloning applies to registered fixed-buffer tables. Current HTTP recv path uses provided buffer rings, so cloning is not an immediate fix for server recv. It becomes relevant if the library adds large registered/fixed buffer pools across many rings.

**TODO:**

- [x] Add `IoUringCaps::registered_buffer_clone`.
- [ ] Document when it applies: registered fixed buffers, not classic provided-buffer-ring recycling.
- [ ] Add wrapper only when a multi-ring fixed-buffer consumer exists.
- [ ] Benchmark startup/registration cost before adding complexity.

## [ ] P3-02: Track io_uring ZC Rx as separate transport mode

**Classification:** Defer.  
**Current state:** Research only.

io_uring ZC Rx is not a drop-in replacement for current provided-buffer recv. It needs NIC support, out-of-band NIC configuration, refill rings, CQE32/mixed CQE setup, and a different lifetime model.

**TODO:**

- [x] Add `IoUringCaps::zc_rx` only as reported capability/diagnostic.
- [ ] Do not mix ZC Rx into current `BufferRing` API.
- [ ] Write a separate design doc before implementation.
- [ ] Prototype only after direct-slot/buffer/CQ ownership model is stable.

## [ ] P3-03: Keep AF_ALG benchmark-gated

**Classification:** Defer.  
**Current state:** Already deferred in socket proposal.

Do not start AF_ALG before socket ownership and Task-ring semantics are clean. AF_ALG has syscall/io_uring overhead and may lose to AESNI for small payloads.

**TODO:**

- [ ] Keep `crypto_alg.cxx` as later optional partition.
- [ ] Add benchmark threshold for payload sizes where AF_ALG wins.
- [ ] Add capability gating and fallback selection only after benchmark proves value.

---

# Ordered implementation plan

## Phase 0 — safety gates before more API surface

- [x] **P0-01** Disable or complete recv bundle decoding.
- [x] **P0-02** Remove hidden submit from `SocketRawRing::get_sqe()`.
- [x] **P0-04** Add `DirectSlotPool` with poison state.
- [x] **P0-05** Add CQ overflow accounting/policy and direct-flow shutdown drain.
- [x] **P0-03** Fix/demote zero-copy-send APIs.

## Phase 1 — coherent async socket layer

- [x] **P1-06** Add `IoUringCaps` matrix.
- [ ] **P1-01** Implement `SocketTaskRing`.
- [ ] **P1-07** Normalize borrowed/owned lifetime contracts.
- [ ] **P1-08** Add cancellation policy for connect/recv/DNS/HTTP.
- [ ] **P1-02** Migrate async plaintext HTTP client/proxy.
- [ ] **P1-03** Migrate DNS transport.
- [ ] Remove/deprecate FileReader socket methods after HTTP/DNS migration.

## Phase 2 — ergonomic server/library surface

- [x] **P1-04** Add `TcpListener` abstraction (Phase 1 done; Phase 2/3 deferred).
- [ ] **P1-05** Add buffer-ring modes: classic, bundle, incremental.
- [ ] **P1-09** Expand benchmark gates.
- [ ] **P2-02** Add owned-path direct-flow variants.
- [ ] **P2-01** Add ring resize wrapper after CQ telemetry.
- [ ] **P2-03** Add poll-first recv policy.

## Phase 3 — opt-in performance tiers

- [ ] **P3-01** Registered buffer cloning wrapper, only with a fixed-buffer multi-ring consumer.
- [ ] **P3-02** ZC Rx design/prototype, separate from current BufferRing.
- [ ] **P3-03** AF_ALG only if benchmark-gated.

---

# Items to avoid for now

- [ ] Do **not** chase ZC Rx before buffer/direct-slot/CQ ownership is airtight.
- [x] Do **not** promote current first-CQE ZC send as an ergonomic Task API. *(P0-03 done)*
- [ ] Do **not** add registered buffer cloning just because the kernel supports it; current server recv path uses provided buffer rings.
- [ ] Do **not** migrate more HTTP/DNS code onto the current FileReader-backed `socket_io.coro` shim.
- [x] Do **not** hide `io_uring_submit()` inside raw/batch helpers. *(P0-02 done)*

---

# Source notes

Repository files checked:

- `TODO.md`
- `SOCKET_IO_PROPOSAL.md`
- `docs/io_uring_direct_file_flow_design.md`
- `src/socket_io/socket_io.cxx`
- `src/socket_io/socket_io_coro.cxx`
- `src/net/http_server.cxx`
- `src/net/client.cxx`
- `src/net/client_async.cxx`
- `src/net/dns/dns.cxx`
- `src/file_io/file_io.cxx`
- `src/uring/flow.cxx`
- `src/uring/uring.cxx`
- `benchmarks/*socket*`, `benchmarks/*http_server*`

External references:

- [io_uring_prep_recv_multishot(3): recv bundle semantics](https://man7.org/linux/man-pages/man3/io_uring_prep_recv_multishot.3.html)
- [io_uring_setup_buf_ring(3): `IOU_PBUF_RING_INC`](https://man7.org/linux/man-pages/man3/io_uring_setup_buf_ring.3.html)
- [io_uring_cq_has_overflow(3)](https://man7.org/linux/man-pages/man3/io_uring_cq_has_overflow.3.html)
- [io_uring_resize_rings(3)](https://man7.org/linux/man-pages/man3/io_uring_resize_rings.3.html)
- [io_uring_clone_buffers(3)](https://man7.org/linux/man-pages/man3/io_uring_clone_buffers.3.html)
- [Linux kernel docs: io_uring zero-copy Rx](https://docs.kernel.org/networking/iou-zcrx.html)
