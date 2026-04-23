# `conflux.work` Design

This document persists the current implementation plan for the new Linux-only
work submission and uring-native execution library.

`io_uring` is a hard runtime requirement for conflux. Hosts where
`io_uring_queue_init*` is blocked or unavailable are unsupported for the server,
file I/O, work-ring tests, and benchmarks. See the top-level `README.md` for
the project requirements and preflight guidance.

## Summary

`conflux.work` is a standalone runtime layer for:

- generic CPU/blocking work on a stealable worker pool
- ring-affine work for `SINGLE_ISSUER` owners
- uring submission programs with linked SQEs, scatter/gather buffers,
  multishot operations, and batched send/receive paths

The API should be composable and pipeable, but naming should favor clarity
over standard-library imitation.

## Runtime Pieces

- `WorkPool`: fixed-size worker pool with per-worker deques and a global inject
  queue
- `RingLane`: single-owner uring lane for owner-thread-only submission/drain
- `IoPlan`: move-only description of one uring submission program
- `IoBuffer` / `BufferList`: non-owning and owning data descriptors for
  contiguous, vectored, mapped, and later registered-buffer I/O

## Important Uring Features

All implemented:

- `IOSQE_IO_LINK` (splice_to_fd linked SQE pairs)
- scatter/gather via `readv` / `writev` (FileReader::readv_into, writev_into)
- mapped-file-backed I/O (FileReader::fadvise_async, madvise_async)
- multishot operation lifecycles (accept + recv multishot in HttpServer)
- send/receive bundles (IORING_RECVSEND_BUNDLE via Config::recv_bundle)
- provided/registered buffers (FixedBufferPool + HttpServer buf_ring)
- `IORING_FEAT_SINGLE_MMAP` (liburing handles transparently)
- `IORING_SETUP_ATTACH_WQ` (Config::attach_wq)
- `IORING_SETUP_NO_MMAP` (Config::no_mmap via io_uring_queue_init_mem)
- `IORING_SETUP_NO_SQARRAY` (Config::no_sqarray)
- `IORING_SETUP_CQE_MIXED` (Config::cqe_mixed)

## Naming Direction

Prefer short, explicit names:

- `value`
- `run_on`
- `then`
- `flat_then`
- `on_error`
- `on_cancel`
- `move_to`
- `start_on`
- `join_all`
- `wait`
- `spawn`

## Integration Direction

After the runtime is stable, migrate:

1. SSE worker launch
2. WebSocket handoff workers
3. proxy offload
4. selected `process` work
5. future HTTP client and file-I/O work

## References

Primary local references:

- `man 7 io_uring`
- `man 2 io_uring_setup`
- `man 2 io_uring_register`
- `man 3 io_uring_prep_msg_ring`
- `man 3 io_uring_register_sync_msg`
- `man 2 futex`
- `man 2 eventfd`
