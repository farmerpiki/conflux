# T2-C: Dedicated IOPOLL Ring for Storage I/O

Date: 2026-05-10
Status: BLOCKED — needs storage_read_bench + buffer ownership design
Effort: 3–5 days
Prerequisite: benchmark proving storage-read bottleneck under HTTP load
Kernel: 5.1+ (IOPOLL exists since initial io_uring; 5.11 adds some `io_uring_enter` extensions)
Device: filesystem and block device must support polling; NVMe needs `poll_queues` configured ([man7.org][1])

## Problem

`FileReader` shares the same io_uring ring as socket operations. File reads
(static serving, file uploads) and socket recv/send/accept compete for CQ
entries. `IORING_SETUP_IOPOLL` eliminates interrupt overhead for storage ops
by polling the device CQ directly — but it can only be set on a ring that
does NOT service socket operations. Kernel returns EINVAL for socket ops
on an IOPOLL ring.

## Current Architecture

```
Ring 0 (per thread)
├── Flags: SINGLE_ISSUER, DEFER_TASKRUN, COOP_TASKRUN, ...
├── Socket ops: accept, recv, send, close
├── File ops: read_fixed, splice_to_fd
├── FixedBufferPool (read-side)
├── PipePool (splice pairs)
└── FileReader (uses same ring)
```

`Ring::init()` (`http_server.cxx:1573`) creates a single ring. `FileReader`
is constructed with `&ring` at line 1689. All operations share one SQ/CQ.

**Key constraint:** `FileReader` is not storage-only. It also contains
`poll_add_oneshot`, `poll_add_multi`, timers/timeouts, socket helpers,
send/recv helpers, DB-related wait support, and file-watch support via
`current_file_reader()`. Moving `FileReader` wholesale to an IOPOLL ring
would submit illegal non-storage ops → EINVAL / API-shape bug.

## Design

### New `IopollFileReader` — storage-only API

Do **not** move `FileReader` to the IOPOLL ring. Keep `FileReader` on the
main ring for poll/timer/socket/DB/file-watch ops. Add a separate narrow
object with v1 surface:

```cpp
struct IopollFileReader {
    // v1: read_nocache_fixed only
    // No open, stat, close, poll, socket, timeout, splice, accept, send, recv, fsync
};
```

Open/stat/path validation happens on the general ring or sync/offloaded
path. The resulting fd is opened with `O_DIRECT` and handed to
`IopollFileReader` for `read_nocache_fixed` only. Close happens on the
owner/general side after all file-ring reads complete.

### Ring Layout

```
Ring 0 (socket/general ring, per thread)
├── Flags: SINGLE_ISSUER, DEFER_TASKRUN, ...
├── Ops: accept, recv, send, close, poll, timer, DB wait, open, stat, close
├── FileReader (general ring helper — unchanged)
└── FixedBufferPool (socket/TLS side)

Ring F (IOPOLL ring, per thread, optional)
├── Flags: IOPOLL, SINGLE_ISSUER
├── Ops: read_nocache_fixed
├── IopollFileReader
└── FixedBufferPool (storage side)
```

```cpp
struct Ring {
    io_uring ring{};
    io_uring file_ring{};
    bool iopoll_ring_enabled{false};

    unique_ptr<FileReader> io;               // main ring: poll, timer, DB, open, misc
    unique_ptr<IopollFileReader> iopoll_io;  // file ring: read_nocache_fixed only

    unique_ptr<FixedBufferPool> socket_or_tls_buffers;
    unique_ptr<FixedBufferPool> iopoll_buffers;
};
```

`current_file_reader()` must NOT point to `IopollFileReader`. Thread-local
current reader stays on the general ring. Access storage-poll via explicit
context:

```cpp
struct CurrentIoContext {
    FileReader* general;
    IopollFileReader* storage_poll;  // nullptr when iopoll disabled
};
```

### Ring Creation

```cpp
if (cfg.iopoll) {
    io_uring_params file_params{};
    file_params.flags = IORING_SETUP_IOPOLL | IORING_SETUP_SINGLE_ISSUER;
    int rc = io_uring_queue_init_params(cfg.file_io_entries, &file_ring, &file_params);
    if (rc < 0) {
        log::warn("iopoll ring creation failed ({}), disabling iopoll storage path", rc);
        iopoll_ring_enabled = false;
    } else {
        iopoll_ring_enabled = true;
        iopoll_io = make_unique<IopollFileReader>(&file_ring, ...);
        log::info("iopoll=enabled ring=ok");
    }
}
```

### Completion Pumping and Continuation Affinity

IOPOLL requires active polling — `io_uring_peek_batch_cqe` alone won't
return completions; `io_uring_enter(..., IORING_ENTER_GETEVENTS)` is
mandatory. ([man7.org][1], [LWN.net][2])

Current `run_loop()` blocks in `io_uring_submit_and_wait(&ring, 1)`. If
file ops are pending on the IOPOLL ring and no socket CQEs arrive, the
thread sleeps on the socket ring and never drives the file ring → stall.

**Strategy: dedicated file I/O thread** with cross-thread completion
delivery back to the owner ring.

**Critical constraint:** IOPOLL completions must NOT resume HTTP/server
coroutines on the file thread. Current `root::Task` readiness resumes the
awaiting coroutine from whichever thread calls `try_set_value()`. If the
file thread dispatches a CQE and completes the task directly,
continuations like `do_streamed_tls_chunk()` resume on the file thread
and mutate HTTP connection state off the socket ring thread — breaking
the single-ring-thread ownership model.

IOPOLL completions must be delivered back to the owning socket/general
ring before resuming any HTTP/server coroutine or mutating
Conn/router/server state. The file thread may only enqueue completion
data; the socket ring thread resumes the coroutine.

```cpp
struct IopollCompletion {
    int owner_ring_id;
    int fd;
    u32 conn_gen;
    FixedBuffer buffer;
    SZ bytes;
    EP error;
};

// file thread:
owner_ring->enqueue_from_file_thread(std::move(completion));
// eventfd write only on empty→non-empty transition to avoid noisy wakeups
if (queue_was_empty)
    eventfd_write(owner_ring->deferred_efd);

// socket ring thread (woken by eventfd):
drain_cross_thread_file_completions();  // drains all queued completions
resume_http_continuation_here();
```

### Buffer Ownership Across Threads

`iopoll_buffers` are registered on the file ring. Once filled, the buffer
is plain memory consumable by the socket ring thread, but ownership must
cross threads cleanly.

For TLS without kTLS:

```text
file thread fills FixedBuffer via read_nocache_fixed
  → socket ring thread receives IopollCompletion
  → socket ring thread calls SSL_write with buffer
  → socket ring thread returns buffer to iopoll pool
```

`FixedBufferPool` needs either thread-safe release or releases proxied
back to the file thread. Current pools assume per-ring ownership — this
is a non-trivial detail requiring design before implementation.

For TLS, an IOPOLL FixedBuffer is returned to the iopoll pool only after
`SSL_write`/`SSL_write_ex` has consumed all bytes from that buffer.
Partial TLS writes retain the buffer plus offset/remaining length on the
owning socket ring. `WANT_WRITE`/`WANT_READ` retries must not release or
mutate the buffer prematurely.

### O_DIRECT Handling

Use `read_nocache_fixed()` (already exists, rounds read length, caps
returned byte count), not plain `read_fixed`.

Requirements:
- Page-aligned read buffers (already true — `FixedBufferPool` uses `aligned_alloc`)
- Query `dio_mem_align` / `dio_offset_align` per device when available;
  conservative fallback: 4096
- Disable IOPOLL **per file** on `EINVAL`/`EOPNOTSUPP`, not globally
  (unless ring creation itself fails)
- Small files stay on mmap/cache path — O_DIRECT bypasses page cache,
  loses readahead/cache benefits for web assets

Default threshold: **64 KiB** (not 4096). Files below this use the
regular ring via existing mmap/splice path.

### Per-Device Negative Cache

Per-file fallback is correct, but repeated `O_DIRECT` / IOPOLL failures
become a hot-path retry tax. Add a small negative cache keyed by
`st_dev` (or mount id if available):

If `EINVAL`/`EOPNOTSUPP` occurs for direct read on device X, skip IOPOLL
for that device for N minutes or until config reload. Log the first
diagnostic; suppress spam after that.

### Static Serving Path Selection

`direct_open_ok` means: file opened with `O_DIRECT`, alignment requirements
known or conservatively assumed, file size >= threshold, device not in
iopoll negative cache, and fd lifetime pinned until all submitted reads
complete.

```cpp
if (iopoll_io && file_size >= iopoll_min_file_bytes && direct_open_ok) {
    return streamed_direct_read_response(...); // read_nocache_fixed on IOPOLL ring
}
return existing mmap/splice path;
```

### Diagnostics

On startup when iopoll enabled:
```text
iopoll=enabled ring=ok direct=ok nvme_poll_queues=unknown
```

On first per-device `EINVAL`/`EOPNOTSUPP`:
```text
iopoll disabled for dev=X: direct I/O or device polling unsupported
```

### Config

```
[io_uring]
iopoll = false                  # create dedicated IOPOLL ring for storage I/O
file_io_entries = 64            # SQ depth for file ring
iopoll_min_file_bytes = 65536   # files below this skip IOPOLL (use mmap/splice)
```

## Acceptance Criteria

1. When `iopoll=true`, storage reads use a separate `IopollFileReader` on
   a ring with `IORING_SETUP_IOPOLL`.
2. `FileReader` stays on the main ring — poll/timer/socket/DB ops unaffected.
3. `IopollFileReader` v1 exposes only `read_nocache_fixed`. No open, stat,
   close, poll, socket, timeout, splice, or fsync on the IOPOLL ring.
4. Graceful fallback if IOPOLL ring creation fails.
5. `read_nocache_fixed` used for storage reads, not plain `read_fixed`.
6. IOPOLL disabled per-device on `EINVAL`/`EOPNOTSUPP` with diagnostic log
   and negative cache to avoid hot-path retry tax.
7. File-ring pumping cannot stall behind `submit_and_wait(&socket_ring, 1)`.
8. IOPOLL completions delivered back to the owning socket/general ring
   before resuming any HTTP/server coroutine or mutating Conn/router/server
   state. The file thread must not resume continuations directly.
9. Client disconnect/cancellation before IOPOLL completion must not
   use-after-free Conn, fd, coroutine frame, or FixedBuffer. Late
   completions dropped by `conn_gen` / owner token mismatch.
10. No regression in `tcp_increment_coro_bench` (--compare-bins, release).

## Benchmark Gate

Must benchmark a path that actually uses `O_DIRECT + read_nocache_fixed`
on the IOPOLL ring. Candidates:

- TLS static serving without kTLS (uses fixed-buffer reads)
- Standalone `storage_read_bench` microbench (O_DIRECT reads on NVMe)

Plain HTTP static file serving via splice does **not** exercise IOPOLL —
splice path is out of scope and mmap is irrelevant.

Benchmark must prove the file ring was exercised:

- IOPOLL file-ring SQ/CQ counters changed
- `read_nocache_fixed` path counter > 0
- splice/mmap path counter == 0 for the benchmark
- `perf stat` includes cs, cycles, instructions, dTLB-load-misses

Hypothesis: 10–30% throughput improvement for direct-read storage workloads
on NVMe. Must pass --compare-bins non-regression.

## Implementation Order

1. Add `storage_read_bench` proving `O_DIRECT + read_nocache_fixed` bottleneck.
2. Design buffer ownership/release across file thread and socket ring thread.
3. Design cancellation/late-completion rules (conn_gen token, fd lifetime).
4. Add `IopollFileReader` as storage-only (no open/stat/poll/close/fsync).
5. Add file-ring thread with coalesced eventfd wakeup.
6. Add cross-thread completion delivery back to owner ring.
7. Add direct-read static path after standalone storage benchmark wins.
8. Test TLS static serving without kTLS.

## Priority

Implement **T2-G registered network send buffers** or **T1-A SEND_ZC
response path** before this if targeting HTTP static-file throughput.
HTTP response path still uses `writev`; zero-copy send only partially
present in primitives. Those directly affect the socket side.

Implement IOPOLL only after a benchmark proves storage-read bottleneck
under realistic HTTP load — not socket send, TLS encryption, splice,
page-cache, or response scheduling.

## Not in Scope

- `fsync` on IOPOLL ring (add only if proven supported and useful on target path).
- `write_fixed` on IOPOLL ring (unnecessary for static serving; add if upload benchmark proves value).
- O_DIRECT for mmap-served files (mmap is already zero-copy, IOPOLL irrelevant).
- Splice path — splice avoids user-space copies; IOPOLL benefit unclear.
- Fixed-file registration on the file ring (later optimization; raw `O_DIRECT` fd + registered buffers enough for v1).

[1]: https://man7.org/linux/man-pages/man2/io_uring_setup.2.html "io_uring_setup(2) - Linux manual page"
[2]: https://lwn.net/Articles/776703/ "Ringing in a new asynchronous I/O API [LWN.net]"
