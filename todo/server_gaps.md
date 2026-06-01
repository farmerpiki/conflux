# Server / Framework Gaps

Status: open TODO

This file tracks only remaining gaps that still affect release confidence or
future branches.

## Open

- [ ] Ring-thread `sched_setaffinity` and `IORING_REGISTER_IOWQ_AFF` application:
  add syscall injection or observable thread/affinity capture if this becomes a
  release gate.
- [ ] Explicitly deferred HTTP streaming upload API: add a bounded-memory request body/multipart
  streaming surface with backpressure and optional spill-to-file. The current
  contract is deliberately bounded in-memory buffering up to `max_body_size`;
  large uploads must not be solved by increasing that cap toward unbounded sizes.
  Do not start before the prerelease API/docs/evidence lanes settle.
- [ ] Keep HTTP handler execution docs aligned with code: handlers run on ring
  threads unless users explicitly offload work.
- [ ] Do not add hidden sync-handler auto-offload.
