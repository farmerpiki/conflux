# io_uring / Socket Remaining Work

Status: open TODO

This file tracks only io_uring/socket work that remains useful for future
implementation.

## Open

- [ ] Poll-first recv benchmark/evidence follow-up.
- [ ] RECV_ZC: deferred until kernel support is mature enough to justify a narrow
  implementation branch.
- [ ] HTTP/static IOPOLL adoption: benchmark-gated; do not wire storage-only IOPOLL
  into HTTP static paths without same-machine evidence.
- [ ] Ring layout/padding: profiling-gated only.
