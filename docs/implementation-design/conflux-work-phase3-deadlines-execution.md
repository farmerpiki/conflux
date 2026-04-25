# `conflux.work` Phase 3 Execution: Deadlines, Timeouts, And Cancellation Sources

Status: complete

Source alignment:

- `conflux-work-api-redesign-deferred-plan.md`
- `conflux-work-phase2-aggregates-execution.md` (Scope baseline)

## Objective

Add time-based cancellation to the carrier layer without introducing a
hidden timer runtime or io_uring dependency:

- `CancelReason::deadline` added to root enum (additive)
- `DeadlineScope`: subclass of `Scope` that fires `cancel(CancelReason::deadline)`
  at a caller-supplied wall-clock or duration deadline
- External timer integration: callers with an existing event loop (io_uring,
  timerfd) call `scope.cancel(CancelReason::deadline)` directly — the carrier
  layer imposes no timer backend

No root contract changes beyond the additive enum value.

## Root Change: `CancelReason::deadline`

Additive: add `deadline` to the `CancelReason` enum in `root.cxx`.

No existing comparisons or switch statements in the codebase enumerate
`CancelReason` exhaustively; the change is source-compatible.

Cancellation reason mapping policy:
- Timer/deadline-triggered cancellation: `CancelReason::deadline`
- User-requested cancellation: `CancelReason::requested`
- Parent scope cancel: `CancelReason::requested` (or `shutdown` for structured
  shutdown — caller's choice via `Scope::cancel(reason)`)
- Abandoned result: `CancelReason::abandoned` (root-internal, unchanged)

## New Module: `conflux.work.carrier.deadline`

File: `src/work/carrier_deadline.cxx`

Imports `conflux.work.carrier.scope`; exports `conflux::work::carrier::DeadlineScope`.

### Design

`DeadlineScope` extends `Scope` with an armed `jthread` timer. The timer
uses `std::condition_variable_any` + `std::stop_callback` so it wakes
immediately when the scope is destroyed (no sleep overshoot after destruction).

```
DeadlineScope::~DeadlineScope()
    → jthread destructor: request_stop() + join()
    → stop_callback fires: cv.notify_one() → timer thread wakes, checks stop → exits
    → then Scope::~Scope() runs
```

This ordering guarantees the timer thread never calls `cancel()` on a
partially-destroyed `Scope`.

### API

```cpp
class DeadlineScope : public Scope {
public:
    explicit DeadlineScope(std::chrono::steady_clock::time_point deadline);

    template<class Rep, class Period>
    explicit DeadlineScope(std::chrono::duration<Rep, Period> timeout)
        : DeadlineScope(std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout)) {}

    ~DeadlineScope();

    DeadlineScope(DeadlineScope&&) = delete;
    DeadlineScope(const DeadlineScope&) = delete;
    DeadlineScope& operator=(DeadlineScope&&) = delete;
    DeadlineScope& operator=(const DeadlineScope&) = delete;

private:
    std::jthread timer_;
};
```

Timer thread body:
```cpp
timer_ = std::jthread{[this, deadline](std::stop_token st) {
    std::mutex mu;
    std::condition_variable_any cv;
    std::stop_callback cb{st, [&cv] { cv.notify_one(); }};
    std::unique_lock lk{mu};
    cv.wait_until(lk, st, deadline, [] { return false; });
    if (!st.stop_requested()) {
        cancel(root::CancelReason::deadline);
    }
}};
```

## External Timer Integration Pattern

For callers with an existing event loop, `DeadlineScope` is not needed:

```cpp
// io_uring example (not in carrier layer — for documentation only)
Scope scope{};
io_uring_timeout_async([&scope] {
    scope.cancel(root::CancelReason::deadline);
});
auto chain = scope.admit(std::move(jh));
```

The carrier layer is timer-agnostic. `DeadlineScope` is a convenience for
callers without an existing timer backend.

## Correctness Matrix

- Initial state not cancelled
- After deadline passes: `is_cancelled()` true, `cancel_reason() == deadline`
- `cancel(requested)` before deadline fires: `cancel_reason() == requested`;
  deadline timer fires no second cancel (idempotent `Scope::cancel()`)
- Already-expired deadline (time_point in past): fires immediately
- `admit` task that completes before deadline: success outcome, no cancel
- `admit` task that outlasts deadline: task receives cancel signal, outcome
  is cancelled with reason `deadline`
- `DeadlineScope` destroyed before deadline: timer thread joins cleanly,
  no cancel fired

## Benchmark Matrix

`release-clang-libcxx`:

- `carrier_a/deadline_scope_fast_path` — task resolves before deadline
  (deadline never fires); measures overhead of arming + disarming timer
- `carrier_a/deadline_scope_cancel_path` — deadline fires; task receives
  cancel signal; measures latency from deadline to cancel delivery

## Immediate Work Items

1. Add `deadline` to `CancelReason` enum in `src/work/root.cxx`.
2. Implement `DeadlineScope` in `src/work/carrier_deadline.cxx`.
3. Register new module in `CMakeLists.txt`.
4. Add focused tests in `tests/work_carrier_phase3_test.cxx`.
5. Add benchmark cases in `benchmarks/work_bench.cxx`.
6. Update deferred plan status for Phase 3.
