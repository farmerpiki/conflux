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

## Exceptions

- `WorkError` base class
- `JoinContextError` for join context/liveness/capability issues
- `FailureError` and `CancelledError` for `value(...)` extraction

## Non-Goals of This Layer

This layer is the root contract surface. It does not define higher-level
combinator/carrier ergonomics, structured concurrency, or sender/receiver
interop adapters.
