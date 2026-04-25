# `conflux.work` Phase 2 Execution: Aggregates And Structured Concurrency Baseline

Status: complete

Source alignment:

- `conflux-work-api-redesign-deferred-plan.md`
- `conflux-work-phase1-carrier-decision.md` (Model A selected)

## Objective

Deliver the first stable aggregate and scope model in the carrier layer
building on `Chain<T>` from Model A:

- `when_all_fast_fail`: fail-fast aggregate variant with documented best-effort
  sibling cancellation contract
- `race`: first-completion combinator with loser ownership/abandon semantics
- `Scope`: parent-triggered cancellation scope for concurrent task groups

No root contract changes. No hidden thread/helper runtime.

## Aggregate API: `when_all_fast_fail`

Extends `carrier_model_a.cxx`.

Semantics:
- In async context: the moment any sibling fails, cancel remaining siblings
  (best-effort) and return the failure; do not wait for loser outcomes.
- In eager context (current): both chains are already resolved at call time;
  the implementation is semantically identical to `when_all`. The API
  communicates the intent that callers should not rely on losers completing
  cleanly after a failure.
- Failure > cancel > success priority (same as `when_all`).

Signature:

```cpp
template<work_value A, work_value B>
    requires(!std::same_as<A, void> && !std::same_as<B, void>)
[[nodiscard]] auto when_all_fast_fail(
    Chain<A>&&, Chain<B>&&) noexcept -> Chain<std::tuple<A, B>>;
```

## Aggregate API: `race`

Extends `carrier_model_a.cxx`.

Semantics:
- Return the "winner" outcome; in async context the loser is abandoned.
- Winner priority: success > failure > cancelled; first arg (`a`) wins ties.
- Loser contract: in async context the loser's root handle would be
  `abandon()`'d. In eager context both chains are already resolved; the loser
  outcome is destructed.
- Winner's `CarrierKind` is preserved in the returned chain.

Signature:

```cpp
template<work_value T>
    requires(!std::same_as<T, void>)
[[nodiscard]] Chain<T> race(Chain<T>&&, Chain<T>&&) noexcept;
```

## Structured Concurrency: `Scope`

New module: `conflux.work.carrier.scope` (`src/work/carrier_scope.cxx`).

### Design

Scope tracks `BasicControl` handles for in-flight tasks. `cancel()` calls
`request_cancel()` on all tracked controls, propagating best-effort sibling
cancellation through `stop_token` to workers.

`admit(BasicJoinHandle<T, C>&&)` combines control tracking with a blocking
join:
1. Extracts the control via `jh.control()` (copies the shared handle).
2. Calls `scope.track(ctrl)` — if scope already cancelled, immediately fires
   `request_cancel()` on the new control.
3. Calls `root::join(std::move(jh))` — blocks until the task resolves
   (possibly cancelled via the scope's cancel signal).

All `track()` and `cancel()` paths are guarded by a mutex for concurrent
access. `cancel()` drains the control vectors under the lock and fires
`request_cancel()` outside the lock to avoid holding the mutex during
potentially contentious root operations.

### API

```cpp
class Scope {
public:
    Scope() noexcept = default;
    ~Scope() = default;
    Scope(Scope&&) = delete;
    Scope(Scope const&) = delete;

    void track(root::TaskControl);
    void track(root::PostedControl);
    void track(root::OperationControl);

    void cancel(root::CancelReason reason) noexcept;

    [[nodiscard]] bool is_cancelled() const noexcept;
    [[nodiscard]] root::CancelReason cancel_reason() const noexcept;

    template<root::work_value T>
    [[nodiscard]] carrier::model_a::Chain<T> admit(root::TaskJoinHandle<T>&&);

    template<root::work_value T, root::progress_capability Owner>
    [[nodiscard]] carrier::model_a::Chain<T> admit(Owner&, root::PostedJoinHandle<T>&&);

    template<root::work_value T, root::progress_capability Driver>
    [[nodiscard]] carrier::model_a::Chain<T> admit(Driver&, root::OperationJoinHandle<T>&&);
};
```

## Benchmark Matrix

All runs on `release-clang-libcxx`:

- `race_a_wins` — race where a resolves to success; b resolves to cancelled
- `race_b_wins` — race where a resolves to cancelled; b resolves to success
- `when_all_fast_fail_both_success` — both success; compare vs `when_all`

## Correctness Matrix

- `when_all_fast_fail` failure priority (a fails) → result is failure
- `when_all_fast_fail` failure priority (b fails) → result is failure
- `when_all_fast_fail` cancel priority (both cancel) → result is cancel
- `when_all_fast_fail` success path
- `race` a-wins on success
- `race` b-wins on success
- `race` a-wins ties (both success)
- `race` failure over cancel
- `race` both fail → a's failure returned
- `race` both cancel → a's cancel returned
- `Scope` track + cancel propagates to joined task
- `Scope` admit after cancel returns cancelled chain
- `Scope` cancel before track immediately fires on new control
- `Scope` admit task control, concurrent cancel during join

## Immediate Work Items

1. Add `when_all_fast_fail` and `race` to `src/work/carrier_model_a.cxx`.
2. Implement `Scope` in `src/work/carrier_scope.cxx`.
3. Register new module in `CMakeLists.txt`.
4. Add focused tests in `tests/work_carrier_phase2_test.cxx`.
5. Add benchmark cases in `benchmarks/work_bench.cxx`.
6. Update deferred plan status for Phase 2.
