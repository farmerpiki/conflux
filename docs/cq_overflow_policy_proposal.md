# CQ Overflow Policy Proposal (P0-05)

Status: proposal
Date: 2026-05-09

## Problem

A lost CQE in a managed flow leaks a buffer slot or direct slot permanently.
The direct-file-flow design treats this as an integration responsibility, but
no enforcement or detection exists at the ring or server level today.

`RingRef::cq_has_overflow()` now wraps `io_uring_cq_has_overflow()`.
`Ring::cq_has_overflow()` wraps the same for owning rings.
That is the only piece currently done.

## What "CQ overflow" means

When the CQ ring fills up, the kernel sets `IORING_SQ_CQ_OVERFLOW` in the SQ
flags and increments the overflow counter in `cq.koverflow`. The dropped CQE
is not in the CQ — it is counted but lost. `io_uring_get_events()` drains the
overflow pending queue (requires `IORING_FEAT_EXT_ARG`).

Managed flows cannot tolerate lost CQEs. A lost close-direct CQE means the
slot stays in `closing` and the file descriptor leaks. A lost recv CQE means
the buffer slot is never recycled.

## Policy Decision

Two options:

**A. Fatal ring error:** On any overflow event, log, drain remaining completions
as best-effort, and shut down the affected ring. Clean restart required.
Simple to reason about; correct for correctness-critical deployments.

**B. Ring quarantine:** Stop submitting new SQEs to the overflowed ring, drain
all pending CQEs, then resume. Allows recovery without restart.
More complex; adds state to the run loop.

**Recommendation:** Start with policy A (fatal). Policy B is a future
optimization once overflow telemetry proves it is worth the complexity.

## Non-Goals

- Do not change CQ size selection at startup (that is a future config item).
- Do not add ring resize logic here (that is P2-01).
- Do not add per-flow CQE accounting; use the ring-level overflow flag.

## Changes Required

### `src/uring/uring.cxx`

Already done: `RingRef::cq_has_overflow()`, `Ring::cq_has_overflow()`.

Add to `Ring`:
```cpp
[[nodiscard]] u32 cq_overflow_count() const noexcept;
```

`cq_overflow_count()` reads `ring_.cq.koverflow` directly (it is mapped
memory, no syscall). This gives the running total for diagnostics.

### `src/net/http_server.cxx`

In the run loop, after each `io_uring_peek_batch_cqe`:
```cpp
if(raw_.ring().cq_has_overflow()){
    // log: CQ overflow — ring integrity lost, shutting down
    // policy A: initiate graceful shutdown
    ring_overflow_fatal();
}
```

`ring_overflow_fatal()` sets a flag that causes `run_loop()` to exit after
draining the current CQE batch. The server restarts (or the process exits,
depending on supervisor policy).

### Shutdown drain

Before `io_uring_queue_exit`, the shutdown path must attempt to drain all
pending CQEs from deferred-close flows. Currently deferred closes are not
drained on normal shutdown either. This is a pre-existing gap that overflow
policy makes urgent.

Add `drain_deferred_closes()` to the shutdown path:
```cpp
void drain_deferred_closes() noexcept {
    // Submit any pending deferred ops.
    // Reap CQEs until no deferred_ops_ remain or timeout.
    // Log any that could not be reaped.
}
```

### Diagnostics

Export overflow count in the server diagnostic log line on shutdown:
```
ring_cq_overflow=N
```

### Stress test

Add a test that configures an undersized CQ (e.g. 4 entries), sends enough
concurrent requests to guarantee overflow, and verifies:
- Server logs the overflow event.
- Server shuts down cleanly (policy A) or recovers (policy B).
- No assertion failures or UB from lost CQEs.

## Verification Targets

- `cq_has_overflow()` returns true when `IORING_SQ_CQ_OVERFLOW` is set.
- `cq_overflow_count()` monotonically increases; never resets on its own.
- Overflow in the server run loop triggers `ring_overflow_fatal()`.
- `drain_deferred_closes()` is called before ring teardown.
- Stress test with undersized CQ passes without UB.

## Implementation Order

1. Add `Ring::cq_overflow_count()` to `uring.cxx`. ← narrow, ready now
2. Add overflow check to `http_server` run loop.
3. Add `ring_overflow_fatal()` shutdown hook.
4. Add `drain_deferred_closes()` to shutdown path.
5. Export overflow count in diagnostics.
6. Add stress test with undersized CQ.
