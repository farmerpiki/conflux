# io_uring / Socket Remaining Work

Status: open inventory

Completed P1/P2 implementation chains were pruned. Use release evidence and
tests for proof of landed work.

## Open

- Poll-first recv benchmark/evidence follow-up.
- RECV_ZC: deferred until kernel support is mature enough to justify a narrow
  implementation branch.
- HTTP/static IOPOLL adoption: benchmark-gated; do not wire storage-only IOPOLL
  into HTTP static paths without same-machine evidence.
- Ring layout/padding: profiling-gated only.

## Done Reference

Completed: multishot accept/recv work, SocketTaskRing accept/client benchmark
coverage, cancellation/close paths, DNS transport cleanup, direct-accept
TCP_NODELAY, registered send buffers, SEND_ZC plumbing, setup-flag fallback, and
buffer-ring recycling fixes.
