# `conflux.work.root` API Reference (Current Public Contract)

This document describes the current public interface and behavior contract of
`conflux.work.root` as implemented in this repository.

## Import

```cpp
import conflux.work.root;
```

## Root Categories

Root async values are split into three categories:

- `Task<T>`: autonomous work
- `Posted<T>`: owner-driven posted work
- `Operation<T>`: driver/state-machine work

Category-specific control and source types:

- `TaskControl`, `TaskSource<T>`
- `PostedControl`, `PostedSource<T>`
- `OperationControl`, `OperationSource<T>`

## Outcome Model

Terminal outcomes are represented by:

- `Success<T>` (`Success<void>` specialization)
- `Failure` (holds non-null `std::exception_ptr`)
- `Cancelled` (`CancelReason::{requested,abandoned,shutdown}`)

And wrapped by:

- `Outcome<T>` / `Outcome<void>`
- `OutcomeKind::{success,failure,cancelled}`

Inspection APIs:

- `kind()`, `is_success()`, `is_failure()`, `is_cancelled()`
- arm accessors: `success()`, `failure()`, `cancelled()`
- total visitor: `visit(F&&)` for `&`, `const&`, `&&`

Result extraction helpers:

- `value(Outcome<T>&&) -> T`
- `value(Outcome<void>&&) -> void`

Error translation:

- `FailureError` for failure arm (`cause()`, `rethrow_cause()`)
- `CancelledError` for cancelled arm (`reason()`)

### Outcome Contract Notes

- `Failure` and `FailureError` normalize null `std::exception_ptr` to a
  non-null sentinel.
- `Outcome<T>` copy assignment uses staged replacement to preserve destination
  state when payload copy throws.
- If internal storage is ever observed valueless-by-exception, implementation
  terminates (`std::terminate()`).
- Accessor precondition violations are caller contract violations.

## Admission APIs

Primary admission APIs:

```cpp
template<work_value T>
std::pair<Task<T>, TaskSource<T>> make_task_source(SubmitOptions = {});

template<work_value T, progress_capability Owner>
std::pair<Posted<T>, PostedSource<T>> make_posted_source(Owner&, PostOptions = {});

template<work_value T, progress_capability Driver>
std::pair<Operation<T>, OperationSource<T>> make_operation_source(Driver&, OperationOptions = {});
```

Control-first admission APIs:

```cpp
template<work_value T>
std::pair<TaskControl, TaskSource<T>> make_task_control_source();

template<work_value T>
std::pair<TaskControl, TaskSource<T>> make_task_control_source(SubmitOptions);

template<work_value T>
std::pair<PostedControl, PostedSource<T>> make_posted_control_source();

template<work_value T>
std::pair<PostedControl, PostedSource<T>> make_posted_control_source(PostOptions);

template<work_value T>
std::pair<OperationControl, OperationSource<T>> make_operation_control_source();

template<work_value T>
std::pair<OperationControl, OperationSource<T>> make_operation_control_source(OperationOptions);
```

Options currently supported:

- `SubmitOptions{ bool enable_cancellation = true; }`
- `PostOptions{ bool enable_cancellation = true; }`
- `OperationOptions{ bool enable_cancellation = true; }`

When `enable_cancellation=false`, `stop_token()` is inert (`stop_possible()==false`),
but `request_cancel()` still marks cancel-request state and runs installed hooks.

## Source Contract

`BasicSource<T, Category>` / `BasicSource<void, Category>` APIs:

- `commit_success(...)`
- `commit_failure(std::exception_ptr)`
- `commit_cancelled(CancelReason)`
- `install_cancel_hook(fn)`
- `stop_token()`

Rules:

- exactly one terminal commit wins
- explicit `commit_cancelled(CancelReason::abandoned)` is a contract violation
  (implementation terminates)
- source destruction without terminal commit performs fallback abandoned cancel

### Commit/Cancel-Hook Race Guarantee

`install_cancel_hook` followed by `commit_*` on a different thread is safe.
The cancel hook is **advisory and independent of terminal commit.** A call to
`request_cancel()` fires the hook at most once but does NOT prevent a
subsequent `commit_success` from winning the terminal commit. Both can happen:
hook fires (cancel requested) and the terminal is committed as success (because
the work completed before the cancel took effect). This is by design —
`request_cancel` is a hint, not a preemption. Callers must not assume that a
fired cancel hook means the source cannot commit success.

Hook installed after terminal commit fires immediately on the calling thread.

**`on_ready` callback and abandon:** when the producer side abandons the control
block without calling `commit_*` (e.g., `~TaskSource` without commit), the
abandon path transitions the control block to a terminal cancelled state and
fires any installed `on_ready` callback, clearing the cycle. Consumers using
`DroppableSlot` rely on this — a drain lambda that owns the join handle will be
invoked and freed by the abandon path, preventing a permanent cycle between the
control block and the drain lambda.

### Cancel-Hook Safety Reference

Cancel hooks must be noexcept. They fire synchronously on the
`request_cancel()` caller's thread. Safe and unsafe operations:

- **Posting to a lane is safe:** hook captures a lane handle or ring reference
  and posts a cleanup job (e.g., `IORING_OP_ASYNC_CANCEL` SQE submission);
  returns immediately. The cleanup job runs on the lane thread.
- **Closing an fd directly is risky:** depends on kernel version and in-flight
  SQE state. Preferred pattern: cancel via SQE and let the CQE handler close
  the fd.
- **TLS shutdown is not safe inline:** `SSL_shutdown` can throw. Wrap in
  `try { } catch (...) {}` inside the hook; actual shutdown belongs in a
  cleanup job posted to the lane.
- **Re-entrancy:** if the calling thread is the lane thread (e.g., a CQE
  handler that triggers cancel), the posted cleanup job must be deferred — use
  a non-reentrant submission path or check `is_ring_thread()` before posting.

## Control Contract

`TaskControl` / `PostedControl` / `OperationControl` provide:

- `request_cancel() -> bool`
- `stop_token() -> std::stop_token`
- `cancel_requested() -> bool`
- `ready() -> bool`
- `state() -> WorkState`
- `can_join_with(CapabilityId) -> bool`

`request_cancel()` returns `true` only for the first successful request before
terminal completion.

Cancel hook semantics:

- single installed hook at most
- first successful cancel request runs hook synchronously
- hook must not throw; throwing hook terminates

## Join, Value, and Join Handles

Join and value APIs:

- `join(Task<T>&&)`
- `join(Owner&, Posted<T>&&)`
- `join(Driver&, Operation<T>&&)`
- same overload set for `TaskJoinHandle<T>`, `PostedJoinHandle<T>`,
  `OperationJoinHandle<T>`
- `value(...)` overloads mirror `join(...)`

Join handles are produced by:

- `into_join_handle(Task<T>&&)`
- `into_join_handle(Posted<T>&&)`
- `into_join_handle(Operation<T>&&)`

Contract behavior:

- `join(...)` on moved-from/non-live object throws `JoinContextError`
- posted/operation joins validate capability identity and throw
  `JoinContextError` on mismatch
- root result and join-handle destructors terminate if still live
  (must be joined, converted, or abandoned explicitly)

## Capability Identity Contract

Capability identity types:

- `CapabilityId { void const* address; void const* type_tag; }`
- `capability_id` CPO (`tag_invoke` customization)
- helper mixin: `capability_id_from_address<Derived>`

`progress_capability` requires `capability_id(cap)` to be available and
`noexcept`, returning `CapabilityId`.

`capability_id_from_address<Derived>` includes per-type static tag plus address
so first-base subobject aliasing does not collapse distinct capability types.

## Abandonment APIs

Abandonment entry points:

- `abandon_to(<result-or-handle>, sink)`
- `guard_abandon(value)` returns `scoped_abandon`

Sink contract (`abandon_sink`):

- sink must be nothrow-move-constructible
- sink must be nothrow-invocable with either:
  - `Outcome<T> const&`, or
  - both `Failure const&` and `Cancelled const&`

Other rules:

- late abandon (already terminal) executes sink on caller thread
- armed abandon executes sink on commit thread
- sink throw is a contract violation and leads to termination
- `scoped_abandon::release()` disarms guard and returns value
- calling `release()` on disarmed guard terminates

### Sink Failure Mode Pattern

Abandon sinks that need to log must not let logging throw. The sanctioned
pattern is a noexcept sink with a try/catch around the logging call:

```cpp
auto sink = root::drop_on_abandon{};  // simplest: silently drop

// If logging is needed:
struct LoggingAbandonSink {
    void operator()(root::Outcome<T> const& out) noexcept {
        try {
            log_orphaned(out);
        } catch (...) {
            // logging threw; silence — terminate is the only alternative
        }
    }
};
```

Do not propagate exceptions from the sink body. `abandon_to` is on the
commit thread; a throwing sink terminates the process.

## Exceptions

- `WorkError` base class
- `JoinContextError` for join context/liveness/capability issues
- `FailureError` and `CancelledError` for `value(...)` extraction

## Implementation Notes

### `MoveOnlyFunction` Inline Buffer

`MoveOnlyFunction<Sig>` uses a **32-byte inline buffer** (default `InlineBytes`
template parameter, aligned to `std::max_align_t`). Callables that fit within
32 bytes are stored inline with no heap allocation. Larger callables heap-allocate.

Cancel hooks and `on_ready` callbacks that capture multiple values (e.g.,
`(fd, ring_handle, context_ptr)`) may exceed the 32-byte limit and heap-allocate.
Callers on hot paths who want to avoid this allocation should measure with the
`callable_erasure_*` benchmarks and restructure captures to fit the buffer.

No public size-hint knob is exposed. If profiling reveals a consistent overflow
pattern, a `MoveOnlyFunction<Sig, InlineCap>` template parameter may be added
as a follow-up.

## Non-Goals of This Layer

This layer is the root contract surface. It does not define higher-level
combinator/carrier ergonomics, structured concurrency, or sender/receiver
interop adapters.
