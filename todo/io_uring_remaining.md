# io_uring / Socket Remaining Work

Status: open TODO

This file tracks only io_uring/socket work that remains useful for future
implementation.

## Open

- Perf/evidence-gated: poll-first recv benchmark/evidence follow-up.
- Deferred: RECV_ZC waits until kernel support is mature enough to justify a narrow
  implementation branch.
- Perf/evidence-gated: HTTP/static IOPOLL adoption; do not wire storage-only IOPOLL
  into HTTP static paths without same-machine evidence.
- Profiling-gated: ring layout/padding only.
