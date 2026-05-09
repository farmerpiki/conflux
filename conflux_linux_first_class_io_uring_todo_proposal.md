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
**Current state:** Partial/unsafe.

Evidence in current repo:

- `src/socket_io/socket_io.cxx:292-309` exposes `submit_recv_multishot(..., bundle=true)` and sets `IORING_RECVSEND_BUNDLE`.
- `src/net/http_server.cxx:3855-3856` enables server recv bundle when config requests it and `IORING_FEAT_RECVSEND_BUNDLE` is present.
- `src/net/http_server.cxx:2898-2909` has an ad-hoc bundle loop over contiguous buffer IDs.
- `src/socket_io/socket_io.cxx:145-167` exposes only simple `buffer_view(id,len)`, `recycle(id)`, and `recycle_batch(ids)`.

The current server code is better than “only reads first buffer,” but the abstraction is still not first-class. Bundle semantics require processing the initial buffer and subsequent contiguous buffers until `cqe->res` bytes are consumed; the CQE gives the first buffer ID, but not the buffer-ring position. The manual recommends tracking a cached head index per buffer ring.

**Decision:** Do not treat current bundle support as production-safe. Either hard-disable `recv_bundle` at config/runtime, or finish bundle support as an explicit `BufferRing` mode with tests.

**TODO:**

- [ ] Add `BufferRingMode::classic_one_cqe_per_buffer`.
- [ ] Add `BufferRingMode::recv_bundle`.
- [ ] Track cached consume head for bundle rings.
- [ ] Add `RecvBundle` decode API that returns all chunks for a CQE.
- [ ] Make recycling bundle-aware; recycle exactly all consumed buffers, once.
- [ ] Add tests for bundle crossing buffer boundaries, wraparound, short first buffer, and multishot end.
- [ ] Until tests pass, force `recv_bundle=false` even if kernel advertises the feature.

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
**Current state:** Not done.

Evidence in current repo:

- `src/socket_io/socket_io.cxx:45-51` calls `io_uring_submit()` inside `SocketRawRing::get_sqe()` if `io_uring_get_sqe()` returns null.

This is surprising in the raw/batch API. Raw callers need control over batching, ordering, backpressure, latency, and linked-chain boundaries. Hidden submit also diverges from the direct-file-flow contract, which expects nonblocking acquisition and whole-chain reservation.

**TODO:**

- [ ] Replace `get_sqe()` with `try_get_sqe() noexcept` that never submits.
- [ ] Add `reserve_sqe_slots(u32 n) noexcept`.
- [ ] Add `get_reserved_sqe() noexcept` for after reservation.
- [ ] Update every raw helper to return `false`/`Err::sq_full` instead of submitting.
- [ ] Audit helpers that need multiple SQEs (`submit_shutdown_close`, `submit_recvmsg_with_timeout`) for all-or-nothing reservation.
- [ ] Keep `submit()` explicit on `SocketRawRing`.

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
**Current state:** Not done.

Evidence in current repo:

- `src/socket_io/socket_io.cxx:227-253` has `DirectFdTable`, but it is only sparse-table registration/install.
- `src/file_io/file_io.cxx:136-190` has `FileHandle` direct-slot ownership warnings, but no shared lease state machine.
- `src/uring/flow.cxx` implements direct-file flow mechanics but still depends on external slot ownership.

The direct-file-flow design explicitly requires external direct-slot leasing and says close failure must poison/leak the slot rather than return it to the generic pool. Without a first-class pool, socket direct accepts, direct socket allocation, fixed-fd install, and direct-file flows can drift into incompatible ownership rules.

**TODO:**

- [ ] Introduce `DirectSlotPool` independent of `DirectFdTable`.
- [ ] Model states: `free`, `leased_empty`, `populated`, `closing`, `poisoned`.
- [ ] Return RAII `DirectSlotLease` for acquired slots.
- [ ] Support “kernel allocated direct slot” adoption from `accept_direct` / `socket_direct_alloc`.
- [ ] Mark slot poisoned on `close_direct` failure.
- [ ] Ensure poisoned slots never re-enter generic free pool.
- [ ] Wire into `uring.flow`, `socket_io`, WebSocket fixed-fd handoff, and direct-close paths.

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

Evidence in current repo:

- `src/net/http_server.cxx:3235-3259` batches CQEs, but no explicit CQ overflow handling was found.
- `src/uring/flow.cxx` has deferred close and CQE accounting tests, but the ring/framework still needs an overflow policy.

Managed flows depend on seeing every CQE. A permanently lost CQE leaks slab state and may leak or poison direct slots. The direct-file-flow design already calls this out as an integration responsibility.

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
**Current state:** Not done.

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

## [ ] P1-04: Add `TcpListener` as a real high-level abstraction

**Classification:** Keep.  
**Current state:** Not done.

Raw helpers exist, but users still need to copy server initialization and rearm patterns. `TcpListener` should not replace the server loop; it should package the reusable socket setup and accept mechanics.

**TODO:**

- [ ] Implement synchronous `TcpListener::bind(...)` first for compatibility.
- [ ] Own listen socket creation, bind/listen, `SO_REUSEADDR`, `SO_REUSEPORT`, `IPV6_V6ONLY`.
- [ ] Optionally install listen fd into a direct slot.
- [ ] Provide `arm_accept_multishot()` / `rearm_accept_multishot()`.
- [ ] Integrate accepted direct-slot bookkeeping with `DirectSlotPool`.
- [ ] Later: add async bind/listen variants if native io_uring bind/listen support is worth exposing.

## [ ] P1-05: Add explicit buffer-ring modes

**Classification:** Keep.  
**Current state:** Not done.

Evidence in current repo:

- `src/socket_io/socket_io.cxx:126` calls `io_uring_setup_buf_ring(..., flags = 0)`.
- Current `BufferRing` assumes one whole buffer per CQE.

Do not make bundle and incremental consumption share the same simple API. They have different ownership and offset rules.

**TODO:**

- [ ] Add `BufferRingMode` to `BufferRingOptions`.
- [ ] Keep classic mode as the default.
- [ ] Add bundle mode with cached head tracking.
- [ ] Add incremental mode with per-buffer offset tracking and `IORING_CQE_F_BUF_MORE` handling.
- [ ] Reject unsupported modes at construction with precise error.
- [ ] Add mode-specific tests and benchmarks.

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
**Current state:** Partial.

Raw `socket_io` has `_borrowed` on some functions, which is good. Task APIs still blur borrowed vs owned lifetime, especially where they wrap FileReader or raw buffer pointers.

**TODO:**

- [ ] Ensure all raw APIs with caller-owned memory use `_borrowed` suffix.
- [ ] For Task APIs, prefer owned/copying semantics unless the function name says borrowed.
- [ ] Add `send_owned(...)` or `send_all_owned(...)` where ergonomics matter.
- [ ] Add `send_static(...)` only if useful for literal/static buffers.
- [ ] Document buffer lifetime through CQE for raw APIs and through task completion for safe Task APIs.
- [ ] For ZC send, define task completion as notification completion, not first CQE.

## [ ] P1-08: Add first-class cancellation policy

**Classification:** Keep.  
**Current state:** Partial.

Evidence in current repo:

- Several current Task sources set `enable_cancellation=false`, including UDP recv paths in `socket_io_coro.cxx`.
- Raw socket helpers already include `submit_cancel_fd` and `submit_cancel_by_ud`, but Task-level semantics are not unified.

**TODO:**

- [ ] Define cancellation policy enum.
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
- [ ] Add recv bundle decode benchmark.
- [ ] Add incremental buffer mode benchmark.
- [ ] Add `SocketTaskRing` vs current FileReader-backed coroutine wrapper benchmark.
- [ ] Add direct slot pool acquire/release/poison benchmark.
- [ ] Add close-direct under SQ-full/deferred-cleanup benchmark.
- [ ] Require `--compare-bins` on server migration steps.

---

# P2 — capability and compatibility polish

## [ ] P2-01: Add ring resize wrapper after CQ telemetry

**Classification:** Keep, narrow.  
**Current state:** Not done.

Ring resizing is useful for network CQ sizing, but it is not a substitute for overflow policy. The man page also places constraints on when resizing is legal: it cannot resize while in overflow, and support is limited by ring setup flags.

**TODO:**

- [ ] Add `Ring::resize(...)` wrapper gated by `IoUringCaps::resize_rings`.
- [ ] Add `grow_cq_to(u32 entries)` convenience API.
- [ ] Reject/return unsupported when ring flags are incompatible.
- [ ] Refuse resizing while CQ overflow is active.
- [ ] Add metrics-driven recommendation: “overflow seen; grow CQ to X next startup / now if legal.”

Suggested API:

```cpp
struct RingSize {
    u32 sq_entries{};
    u32 cq_entries{};
};

expected<void, Err> Ring::resize(RingSize) noexcept;
expected<void, Err> Ring::grow_cq_to(u32 entries) noexcept;
```

## [ ] P2-02: Add owned-path variants for direct-file flow

**Classification:** Keep.  
**Current state:** Not done.

Evidence in current repo:

- `src/uring/flow.cxx:674-680` rejects all flow submissions when the borrowed-path lifetime contract is not stable.
- The public type is `BorrowedPath`; no owned-path variant was found.

This is safe but too limiting for SQPOLL and old/no-submit-stable environments. A first-class API should let users pay one owned-path copy and still use the flow abstraction.

**TODO:**

- [ ] Add `open_direct_owned_path(...)`.
- [ ] Add `with_direct_file_owned_path(...)`.
- [ ] Store owned path in fixed-capacity per-flow storage or cold allocation outside submit/CQE hot path.
- [ ] Allow borrowed path only when `submit_stable && !sqpoll` or caller explicitly promises open-CQE lifetime.
- [ ] Add tests for SQPOLL/no-submit-stable admission behavior.

## [ ] P2-03: Add poll-first recv/send policy wrapper

**Classification:** Keep.  
**Current state:** Low-level flags exist; socket policy does not.

Evidence in current repo:

- `src/uring/uring.cxx` exposes low-level `recvsend_poll_first` constants.
- `socket_io` does not expose a recv-arm policy.

**TODO:**

- [ ] Add `RecvArmPolicy` to `submit_recv_multishot` or a higher-level arm options struct.
- [ ] Track last CQE flags such as socket-nonempty to drive `auto_from_last_cqe`.
- [ ] Benchmark idle-heavy vs busy sockets.
- [ ] Keep default as current behavior until benchmark proves otherwise.

Suggested shape:

```cpp
enum class RecvArmPolicy : u8 {
    try_now,
    poll_first_when_idle,
    auto_from_last_cqe,
};
```

---

# P3 — backlog / research / opt-in performance tiers

## [ ] P3-01: Track registered buffer cloning, but do not implement now

**Classification:** Defer.  
**Current state:** Not relevant to current provided-buffer server path.

Registered buffer cloning applies to registered fixed-buffer tables. Current HTTP recv path uses provided buffer rings, so cloning is not an immediate fix for server recv. It becomes relevant if the library adds large registered/fixed buffer pools across many rings.

**TODO:**

- [ ] Add `IoUringCaps::registered_buffer_clone`.
- [ ] Document when it applies: registered fixed buffers, not classic provided-buffer-ring recycling.
- [ ] Add wrapper only when a multi-ring fixed-buffer consumer exists.
- [ ] Benchmark startup/registration cost before adding complexity.

## [ ] P3-02: Track io_uring ZC Rx as separate transport mode

**Classification:** Defer.  
**Current state:** Research only.

io_uring ZC Rx is not a drop-in replacement for current provided-buffer recv. It needs NIC support, out-of-band NIC configuration, refill rings, CQE32/mixed CQE setup, and a different lifetime model.

**TODO:**

- [ ] Add `IoUringCaps::zc_rx` only as reported capability/diagnostic.
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

- [ ] **P1-04** Add `TcpListener` abstraction.
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
- [ ] Do **not** promote current first-CQE ZC send as an ergonomic Task API.
- [ ] Do **not** add registered buffer cloning just because the kernel supports it; current server recv path uses provided buffer rings.
- [ ] Do **not** migrate more HTTP/DNS code onto the current FileReader-backed `socket_io.coro` shim.
- [ ] Do **not** hide `io_uring_submit()` inside raw/batch helpers.

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
