# Server / Framework Gaps

Status: open inventory

Completed implementation and test-pass chains were pruned. This file tracks
only remaining gaps that still affect release confidence or future branches.

## Open

- Ring-thread `sched_setaffinity` and `IORING_REGISTER_IOWQ_AFF` application:
  add syscall injection or observable thread/affinity capture if this becomes a
  release gate.
- Keep HTTP handler execution docs aligned with code: handlers run on ring
  threads unless users explicitly offload work.
- Do not add hidden sync-handler auto-offload.

## Done Reference

Completed: direct-accept TCP_NODELAY, busy-poll/ring-core config, registered
send buffers, SEND_ZC lifecycle seams, atomic async publish, root allocation
counters/pools, JSON arena/incremental coverage, router concepts/diagnostics,
adaptive io_uring setup fallback, shutdown force-close, HTTP helper/parser/body
coverage, and client redirect coverage.
