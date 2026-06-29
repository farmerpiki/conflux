# io_uring / Socket Remaining Work

Status: open TODO

This file tracks only io_uring/socket work that remains useful for future
implementation.

## Open

- Deferred: RECV_ZC production backend. Capability probing and reserved payload
  ownership shape exist, but no production `IORING_OP_RECV_ZC` SQE path is wired.
- Perf/evidence-gated: HTTP/static IOPOLL adoption. Storage-only IOPOLL exists
  in `conflux.file_io.iopoll`; do not wire it into HTTP static paths without
  same-machine evidence that the storage-read bottleneck justifies the extra
  ring and fixed-buffer constraints.
- Profiling-gated: ring hot/cold layout or padding changes only.

## Verified Done

- Poll-first recv policy support is implemented and covered by policy tests;
  future work here is benchmark/evidence capture, not initial implementation.
- Storage-only IOPOLL primitive, tests, and storage-read benchmark gate exist.
