# io_uring SEND_ZC CQE lifecycle

`IORING_OP_SEND_ZC` can complete as either:

- a data CQE with `IORING_CQE_F_MORE`, followed later by a notification CQE;
- a data CQE without a later notification;
- an error data CQE, which may still require waiting for the notification CQE when `MORE` is set.

The HTTP server keeps that lifecycle in `SendZcCqeState` and routes every SEND_ZC CQE through
`observe_send_zc_cqe(...)` before applying server side effects.  The helper is intentionally
side-effect-light: it updates SEND_ZC counters, advances notification state, and returns one action:
complete the response, resubmit the remaining bytes, close after error, or close after the pending
notification arrives.

This keeps the kernel-dependent CQE handling testable without a live ring.  The deterministic tests in
`tests/send_zc_lifecycle_test.cxx` cover:

- partial data CQE + notification before resubmit;
- full data CQE + copied notification + adaptive SEND_ZC disable;
- no-notification CQEs that directly resubmit or complete;
- `ENOMEM` accounting while waiting for the required notification;
- queued close while a notification is still pending.

The send path still uses live-kernel benchmarks for throughput/threshold decisions.  The helper only
covers control-flow correctness and counter accounting.
