# Performance Ideas — conflux (April 2026)

Based on three independent ecosystem reviews of C++26 + io_uring best practices, cross-referenced against what the codebase already does. Module-based C++26 throughout.

---

## Current State (inventory)

| Area | Status |
|------|--------|
| `SINGLE_ISSUER` + `DEFER_TASKRUN` + `COOP_TASKRUN` | Done (config-gated) |
| `SQPOLL` | Done (config-gated, off by default) |
| `IORING_SETUP_IOPOLL` | Done for storage-only O_DIRECT file rings (`IopollStorageRing` / `IopollFileReader`); HTTP static path integration remains separate and benchmark-gated |
| Registered files (sparse) | Done |
| Registered buffers (sparse, `FixedBufferPool`) | Done for file I/O + HTTP send (plain small responses via `FixedBufferPool` send buffers; `submit_send_fixed_borrowed`) |
| Provided buffer rings for recv | Done |
| `IORING_RECVSEND_BUNDLE` | Done (config-gated) |
| Multishot accept + recv | Done |
| Splice for file serving | Done |
| Zero-copy send (`SEND_ZC`) | Experimental. Implementation exists for HTTP plain and mapped-file send paths (`queue_send` / `queue_send_mapped` → `submit_send_zc_borrowed` above threshold, `IORING_CQE_F_NOTIF` handling, send-zc counters); mapped headers are sent normally before ZC body send; TLS cannot use SEND_ZC directly. Performance status remains pending non-loopback, ZC-capable NIC evidence. |
| Zero-copy recv (`RECV_ZC`) | Missing |
| `SO_BUSY_POLL` / `SO_PREFER_BUSY_POLL` | Done — `busy_poll_us` / `prefer_busy_poll` config fields; applied per accepted socket (`http_server.cxx:2397-2400`) |
| `TCP_QUICKACK` | Done — set unconditionally per accepted socket (`http_server.cxx:2396`) |
| `TCP_NODELAY` on accepted conns | Done — set unconditionally per accepted socket (`http_server.cxx:2395`) |
| CQE batch drain (`peek_batch_cqe`, 256) | Done |
| File I/O drain — batch `peek_batch_cqe` | Done — 32-CQE batch via `io_uring_peek_batch_cqe` + single `io_uring_cq_advance` (`file_io.cxx:3015-3025`) |
| Ring thread affinity | Done — `ring_core` config field; `sched_setaffinity` in `run_loop` (`http_server.cxx:3688-3698`) |
| io-wq affinity | Done — `worker_core_base` config field; `IORING_REGISTER_IOWQ_AFF` in `run_loop` (`http_server.cxx:3688-3698`) |
| Huge pages / `MADV_HUGEPAGE` | Done — file I/O slabs (`file_io.cxx:109`), socket recv slab when `huge_pages=true` (`socket_io.cxx:173`), HTTP server buffer ring (`http_server.cxx:1683`) |
| `MADV_DONTFORK` on ring buffers | Done — file I/O (`file_io.cxx:108`), socket recv slab (`socket_io.cxx:174`) |
| `alignas(64)` on hot structs | Done for `Conn` (`alignas(64)`), `Worker` (`alignas(64)`), MPMC ring `head_`/`tail_`; `Ring` has no alignas — verify field grouping with `perf c2c` before further padding |
| Coroutine frame custom allocator | Done (`CONFLUX_WORK_CORO_FRAME_POOL` CMake flag) — `EagerChain` uses a thread-local bump arena; `Task<T>` promise frames use pooled mmap-backed buckets with PMR fallback; off by default and sanitizer-safe |
| Work-stealing pool | Global injection via lock-free MPMC ring (`inject_ring_`); per-worker local queues + stealing still use `std::mutex`; `admission_mtx_` gates enqueue/admission |
| `memory_order_seq_cst` in hot paths | Present in WorkPool wake/park protocol (two `atomic_thread_fence(seq_cst)` forming a fence pair for `parked_` visibility before `pending_` check); verify necessity with correctness reasoning before weakening |

---

## Ideas by Impact Tier

### Tier 1 — Structural (weeks; 2×+ potential)

**T1-A: Zero-copy network send via `IORING_OP_SEND_ZC`** — **Implemented, experimental**
Core path landed for plain and mapped responses: `queue_send` / `queue_send_mapped` use `submit_send_zc_borrowed` above configured threshold, with `IORING_CQE_F_NOTIF` handling, send-zc counters, normal-header/mapped-body sequencing, and fallback regular sends. TLS path intentionally cannot use SEND_ZC directly. Treat threshold and throughput conclusions as experimental until evidence comes from non-loopback traffic over ZC-capable NICs and shows non-copied notifications.

**T1-B: Zero-copy recv via `IORING_OP_RECV_ZC`**
Provided buffer rings already remove the copy on the ring side; `RECV_ZC` goes further — DMA directly into user buffers. Kernel 6.20/7.0 improves large-buffer support. Hold off until 6.20 is stable; design the recv buffer abstraction now so it's swappable.

**T1-C: `std::execution` (P2300) scheduler integration**
Current `root::Task<T>` is a hand-rolled coroutine type backed by `FlowSource`. The hot path allocates a coroutine frame per request. A P2300 `io_uring_scheduler` sender would allow compile-time pipeline construction, stack-allocated operation states for synchronous completions, and structured cancellation via stop tokens — eliminating per-request heap allocation on the steady-state path. NVIDIA stdexec is the reference implementation. This is the largest architectural change.

**T1-D: Lock-free work queues** — **Partially done**
Global injection path now uses a lock-free Vyukov-style MPMC ring (`inject_ring_`). Per-worker local queues still use `std::mutex`; stealing still locks worker deques; `admission_mtx_` still gates enqueue/admission. Profile local deque locks, steal path, and `admission_mtx_` under real HTTP load before replacing — the futex + `_mm_pause` spin already amortizes at low contention.

---

### Tier 2 — Medium (days; 10–40% gains where applicable)

**T2-A: `TCP_NODELAY` on every accepted connection** — **Done** (`http_server.cxx:2395`)
Set unconditionally per accepted socket. No longer a gap.

**T2-B: `SO_BUSY_POLL` + `SO_PREFER_BUSY_POLL` per accepted socket** — **Done** (`http_server.cxx:2397-2400`)
`busy_poll_us` and `prefer_busy_poll` config fields; applied via `setsockopt` after each accept. Off by default. Pair with `IORING_SETUP_SQPOLL` for the full zero-syscall path.

**T2-C: `IORING_SETUP_IOPOLL` for storage rings** — **Primitive done**
`IopollStorageRing` / `IopollFileReader` landed as a storage-only O_DIRECT read surface using a dedicated `IORING_SETUP_IOPOLL` ring. `FileReader` remains on the general ring for poll/timer/socket/DB/file-watch work. Remaining work is not the primitive; it is benchmark-gated HTTP/static-path adoption and any per-device fallback policy.

**T2-D: `file_io.cxx` drain loop — batch `peek_batch_cqe`** — **Done** (`file_io.cxx:3015-3025`)
32-CQE batch via `io_uring_peek_batch_cqe` + single `io_uring_cq_advance` per burst. No longer suboptimal.

**T2-E: Ring thread + worker affinity** — **Done** (`http_server.cxx:3688-3698`)
`ring_core` (sched_setaffinity for ring thread) and `worker_core_base` (IORING_REGISTER_IOWQ_AFF for io-wq) config fields. Both default to -1 (disabled). Pair with `isolcpus` in kernel params for full isolation.

**T2-F: `alignas(64)` on hot structs** — **Mostly done**
`Conn` and `Worker` both have `alignas(64)`. `Ring` has no alignas — it is per-thread so false sharing is less likely, but hot/cold field grouping should still be verified with `perf c2c` under load.

**T2-G: Registered buffers for network send (headers + small bodies)** — **Done**
`FixedBufferPool` send buffers landed. `queue_send` acquires registered buffer when `send_buffers && send_fixed_buffers_supported && len <= send_buffers->slab_bytes()`, uses `submit_send_fixed_borrowed`; falls back to normal `submit_send_borrowed` if unavailable.

---

### Tier 3 — Quick wins (hours; correctness + marginal gains)

**T3-A: `TCP_NODELAY` on accepted sockets** — **Done** (see T2-A).

**T3-B: `TCP_QUICKACK` on accepted sockets** — **Done** (`http_server.cxx:2396`)
Set unconditionally after accept. Note: Linux resets `TCP_QUICKACK` per-send; the send path does not re-set it, which is acceptable for most request-response workloads but may matter for multi-send flows.

**T3-C: `IORING_SETUP_NO_SQARRAY`** — **Done** (config default `no_sqarray=true`; stripped via EINVAL fallback on older kernels)

**T3-D: Hugepage backing for provided recv buffers** — **Done** (`http_server.cxx:1683`, `socket_io.cxx:173`)
HTTP server buffer ring uses `huge_pages=true` → `MADV_HUGEPAGE` on the slab. Socket recv slab also supports hugepage via `BufferRingOptions.huge_pages`. File I/O slabs similarly (`file_io.cxx:109`).

**T3-E: Coroutine frame pool** — **Done** (`CONFLUX_WORK_CORO_FRAME_POOL`)
`EagerChain` keeps the per-thread monotonic bump arena. `Task<T>` promise frames now allocate through size-bucketed mmap-backed pools with a process-lifetime PMR fallback, so the request-path `TaskPromise<T>` gap is closed when the option is enabled. Sanitizer builds keep the safe fallback path.

**T3-F: `madvise(MADV_DONTFORK)` on ring buffers** — **Done** (`file_io.cxx:108`, `socket_io.cxx:174`)
Applied after slab allocation in both file I/O and socket recv paths.

**T3-G: `IORING_FEAT_*` capability probing at startup** — **Done** (`detect_caps` in `uring.cxx` uses `io_uring_get_probe_ring`; `caps_to_log_string` logs all feature/op bits at ring 0 startup; HTTP server config logging now reports requested/active/stripped setup-flag sets after EINVAL fallback).

---

## Quick Win Summary

All original Tier 3 quick wins are implemented. Remaining open items:

| ID | What | Where | Effort |
|----|------|--------|--------|
| ~~T3-G~~ | ~~`io_uring_get_probe()` capability log~~ | ~~Done~~ (`detect_caps` + `caps_to_log_string` + requested/active/stripped setup-flag log) | — |
| ~~T2-F~~ | ~~`alignas(64)` on `Conn` + `Worker`~~ | ~~Done~~; `Ring` hot/cold layout remains | — |
| ~~T2-G~~ | ~~Registered buffers for network send~~ | ~~Done~~ (`FixedBufferPool` send buffers) | — |
| T1-A | Zero-copy HTTP response send (`SEND_ZC`) | Implementation done for plain + mapped responses; TLS remains intentionally excluded; perf/threshold status experimental until non-loopback ZC-capable NIC evidence exists | evidence-gated |
| ~~T1-D~~ | ~~Lock-free global injection~~ | ~~Done~~ (MPMC ring); local queues/stealing/admission_mtx_ still mutex-based | — |
| ~~T3-E~~ | ~~Coroutine frame pool for `TaskPromise<T>`~~ | ~~Done~~ (`work/root.cxx`) | — |
| ~~T2-C~~ | ~~`IORING_SETUP_IOPOLL` for storage rings~~ | ~~Done~~ (`IopollStorageRing` / `IopollFileReader`); HTTP adoption still needs benchmark evidence | — |
| T1-B | Zero-copy recv (`RECV_ZC`) | `socket_io.cxx`, recv path | wait for kernel ≥ 6.20 |

Item T1-C (P2300 scheduler) is intentionally deferred — multi-week architectural change. T1-D partially landed (MPMC injection ring); remaining local queue/steal lock removal is deferred pending contention profiling. T2-C is no longer a missing primitive; only measured consumers remain.

---

## Measurement Plan

Do not implement T1/T2 items without a benchmark harness first:
- Baseline: `benchmarks/tcp_increment_coro_bench` (incl. `fr/*`, `str/*`, `parallel_4` variants) for latency/throughput. (`tcp_parallel_coro_bench` was deleted; `str/parallel_4` is the N=4 coverage.)
- Storage: `benchmarks/file_copy_coro_bench` for registered-buffer gains.
- Profiling: `perf stat -e cache-misses,LLC-load-misses,dTLB-load-misses,cs` on the server process under `wrk`/`h2load` load.
- io_uring counters: `/proc/self/fdinfo/<ring_fd>` fields `sq_dropped`, `cq_overflow`, `sq_busy` for tuning batch size and ring depth.

Never claim a gain without numbers from the same hardware under realistic HTTP load.
