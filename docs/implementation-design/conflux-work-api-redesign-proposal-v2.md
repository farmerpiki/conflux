# `conflux.work` API Redesign (Proposal V2)

Status: proposal from review synthesis

This document replaces
[`conflux-work-api-redesign-revised.md`](./conflux-work-api-redesign-revised.md)
as the next design candidate.

It incorporates the highest-priority findings from review and narrows the
contract where the previous draft was over-promising.

The locked pre-plan decisions remain in
[`conflux-work-api-redesign-decisions.md`](./conflux-work-api-redesign-decisions.md).

## Summary

This redesign mirrors three execution modes that already exist underneath
conflux. It does not introduce a new three-way split for its own sake.

The public root categories map directly onto the existing internal modes:

- autonomous scheduled work
- explicitly posted owner work
- driver / CQE / state-machine operations

The core proposal is:

- keep those three root categories explicit
- keep rejection synchronous at admission
- make owner/driver progress requirements explicit
- make result destruction semantics explicit: a live unconsumed result behaves
  like `std::thread` and terminates
- separate copyable control handles from typed join handles
- keep cancellation best-effort and explicit
- keep affinity semantic, but do not finalize the coroutine-carrier mechanism
  yet
- omit deadlines, parent-cancellation scopes, droppable streams, and aggregate
  surface details until their primitives are designed

## What Is Locked Here

This proposal locks the root async model and the lifecycle model.

It does not lock:

- the final coroutine / combinator carrier shape
- the exact hop primitive names
- timer / deadline APIs
- structured-concurrency APIs

## Root Categories

### `Task<T>`

`Task<T>` is autonomous scheduled work.

Use it for:

- `WorkPool` jobs
- CPU work
- blocking adapters running on worker threads
- completions that do not require a specific owner or subsystem driver to pump

### `Posted<T>`

`Posted<T>` is explicitly posted owner work.

Use it for:

- `RingLane` queue-node work
- owner-thread closures
- other "run this on that owner" work where the owner drains the queue

### `Operation<T>`

`Operation<T>` is a driver / CQE / state-machine operation.

Use it for:

- `file_io`
- `db`
- future subsystems whose progress is driven by CQEs, polling phases, or
  subsystem-owned state machines

### Why Three Public Categories

The split is public because conflux already has three different progress models
underneath:

- autonomous scheduler progress
- posted owner progress
- subsystem driver progress

Those models differ in:

- who makes forward progress
- where completion is committed
- what cancellation can do before start
- what a blocking join must pump

The previous two-way split hid one real distinction too many.

### Consumer-Side Generic Code

The `Posted<T>` vs `Operation<T>` split is primarily about admission,
production, progress, and cancellation mechanics.

Consumer-side generic code that only needs:

- `control()`
- `join(ctx, ...)`
- `value(ctx, ...)`

should program against a common owner-driven concept rather than branching on
the concrete type name.

The root types stay separate because subsystem authors need the distinction
even when generic consumers do not.

## Outcome Model

Admission failure is synchronous and no async value exists yet.

Accepted work can terminate only as:

- success
- failure
- cancelled

```cpp
namespace conflux::work {

template<class T>
struct Success {
    T value;
};

template<>
struct Success<void> {};

struct Failure {
    std::exception_ptr error;
};

enum class CancelReason : uint8_t {
    requested,
    shutdown,
    external,
};

struct Cancelled {
    CancelReason reason;
};

template<class T>
using Outcome = std::variant<Success<T>, Failure, Cancelled>;

enum class RejectReason : uint8_t {
    stopped,
    queue_full,
    owner_violation,
    resource_exhausted,
    notify_failed,
};

} // namespace conflux::work
```

Notes:

- `Success<T>` is always a distinct wrapper, including for non-`void` `T`.
- This proposal assumes exceptions-enabled builds for the core failure channel.
- Domain APIs that prefer status values may still use payload shapes such as
  `Task<expected<U, E>>`.
- `CancelReason::deadline` and `CancelReason::parent` are intentionally omitted
  until there are real deadline and structured-concurrency primitives.

### `RejectReason`

`RejectReason` means admission failed and no async value was published.

`notify_failed` is defined narrowly:

- the runtime could not publish or notify the owner/driver
- no async value exists
- retry is safe and cannot duplicate already-published work

`owner_violation` means a runtime precondition for owner/driver use was
violated at a boundary that cannot be proven statically.

It is admission-time only.

It does not describe misuse of `join(...)` / `value(...)` after an async value
already exists.

Examples:

- posting to a thread-bound owner from a context that does not hold the right
  owner token
- trying to start a thread-bound owner/driver operation from a context the
  subsystem forbids

## Admission APIs

Admission is explicit and typed.

Working shapes:

```cpp
template<class Exec, class Fn>
auto submit(Exec& exec, Fn&& fn)
    -> std::expected<Task<submit_result_t<Fn>>, RejectReason>;

template<class Owner, class Fn>
auto post(Owner& owner, Fn&& fn)
    -> std::expected<Posted<post_result_t<Fn>>, RejectReason>;
```

Callable form selection for `submit(...)`, `post(...)`, and `spawn(...)`:

- a callable may be invocable either as `fn()` or as `fn(CancelToken)`
- exactly one of those forms must be valid
- if both forms are valid, the call is ill-formed and the caller must
  disambiguate explicitly
- `submit_result_t<Fn>` / `post_result_t<Fn>` mean the result type of that
  unique accepted invocation form

Subsystem-owned operations follow the same rule:

- if the subsystem cannot arm, register, or publish the operation yet, that is
  synchronous `RejectReason`
- once the operation exists, later problems are `Failure` or `Cancelled`

Examples:

- `file_io` SQE/resource exhaustion before publication is rejection
- `db` refusal before a query operation is armed is rejection
- a kernel, SQL, protocol, or remote error after admission is failure

## Producer-Side Construction

The previous revised draft described `Operation<T>` only from the consumer
side. That is not enough.

Each accepted root async value has a producer-side source object.

Working shapes:

```cpp
template<class T>
class TaskSource {
public:
    bool commit_success(Success<T> value);
    bool commit_failure(std::exception_ptr error);
    bool commit_cancelled(CancelReason reason);
    bool install_cancel_hook(std::move_only_function<void(CancelReason)> fn);
};

template<class T>
class PostedSource {
public:
    bool commit_success(Success<T> value);
    bool commit_failure(std::exception_ptr error);
    bool commit_cancelled(CancelReason reason);
    bool install_cancel_hook(std::move_only_function<void(CancelReason)> fn);
};

template<class T>
class OperationSource {
public:
    bool commit_success(Success<T> value);
    bool commit_failure(std::exception_ptr error);
    bool commit_cancelled(CancelReason reason);
    bool install_cancel_hook(std::move_only_function<void(CancelReason)> fn);
};

template<class T>
std::pair<Task<T>, TaskSource<T>> make_task_source();

template<class T>
std::pair<Posted<T>, PostedSource<T>> make_posted_source();

template<class T>
std::pair<Operation<T>, OperationSource<T>> make_operation_source();
```

Producer-side rules:

- first terminal commit wins
- later terminal commits are ignored and return `false`
- `request_cancel()` is not itself terminal completion
- cancel hooks are advisory and category-specific

Cancel-hook contract:

- at most one cancel hook may be installed
- installing after terminal completion returns `false`
- if cancellation was already requested but no terminal outcome has yet been
  committed, `install_cancel_hook(...)` may invoke the hook inline before
  returning `true`
- the hook is invoked at most once
- the hook is never invoked concurrently with itself
- terminal commit racing with cancellation uses first terminal commit wins
- no hook invocation occurs after terminal completion wins
- hook execution must not depend on internal control-block locks remaining held

## Lifetime Model

### Root Result Objects

`Task<T>`, `Posted<T>`, and `Operation<T>` are move-only consuming tokens.

A live root result object must be exactly one of:

- joined / awaited
- moved elsewhere
- converted into a background join handle
- explicitly abandoned to an error sink

Destroying a live root result object calls `std::terminate`, exactly like
destroying a joinable `std::thread`.

This includes exception unwinding.

That behavior is a hard contract, not a debug-only assertion.

The previous draft called this "program error". This proposal makes the
contract explicit.

### Copyable Control Handles

Copyable control handles are observation/cancellation handles only.

Working shapes:

- `TaskControl`
- `PostedControl`
- `OperationControl`

Those handles are intentionally type-erased with respect to `T`.

Every root result object and every background join handle exposes `control()`
to obtain the corresponding copyable control handle.

They support:

- `request_cancel(CancelReason = CancelReason::requested)`
- `cancel_requested()`
- `ready()`
- category tag / minimal state snapshot

They do not surface the success value.

### Typed Join Handles For Background Work

Background-start APIs return typed join handles, not bare copyable control
handles.

Working shapes:

```cpp
template<class T>
class TaskJoinHandle;

template<class T>
class PostedJoinHandle;

template<class T>
class OperationJoinHandle;
```

Those join handles are:

- move-only
- typed by `T`
- join-capable
- able to expose a copyable control handle

They do not create progress by themselves.

For owner-driven categories, converting to or creating a background join handle
changes lifetime ownership only:

- `PostedJoinHandle<T>` still requires owner pumping
- `OperationJoinHandle<T>` still requires whatever progress-driving capability
  the subsystem defines for terminal publication

Their destructor also calls `std::terminate` if still live.

Root result objects may also be converted explicitly into background join
handles:

```cpp
template<class T>
TaskJoinHandle<T> background(Task<T>&& task);

template<class T>
PostedJoinHandle<T> background(Posted<T>&& posted);

template<class T>
OperationJoinHandle<T> background(Operation<T>&& op);
```

Working shapes:

```cpp
template<class Exec, class Fn>
auto spawn(Exec& exec, Fn&& fn)
    -> std::expected<TaskJoinHandle<submit_result_t<Fn>>, RejectReason>;

template<class Owner, class Fn>
auto spawn(Owner& owner, Fn&& fn)
    -> std::expected<PostedJoinHandle<post_result_t<Fn>>, RejectReason>;
```

Subsystem-owned operations should also expose background forms when that mode
is needed:

```cpp
namespace db {

template<class... Args>
auto start_query(Connection& conn, Args&&... args)
    -> std::expected<OperationJoinHandle<QueryResult>, RejectReason>;

} // namespace db
```

This is important for router / HTTP / connection-lifetime flows. A copyable
cancel handle alone is not enough; the design also needs a typed final-outcome
path.

### Explicit Abandon

If a caller truly wants no later `T`, it must say so explicitly:

```cpp
template<class T, class Sink>
void abandon_to(Task<T>&&, Sink&& sink);

template<class T, class Sink>
void abandon_to(Posted<T>&&, Sink&& sink);

template<class T, class Sink>
void abandon_to(Operation<T>&&, Sink&& sink);

template<class T, class Sink>
void abandon_to(TaskJoinHandle<T>&&, Sink&& sink);

template<class T, class Sink>
void abandon_to(PostedJoinHandle<T>&&, Sink&& sink);

template<class T, class Sink>
void abandon_to(OperationJoinHandle<T>&&, Sink&& sink);
```

This is not the normal server pattern, but it is the explicit escape hatch.

`abandon_to(...)` keeps running work alive and has these semantics:

- `Success<T>` is discarded
- `Failure` is delivered to the sink
- `Cancelled` is silent unless the sink opts into cancelled delivery
- the sink runs on the context that commits the terminal outcome
- `abandon_to(...)` does not create progress or marshalling; callers that need
  a different context must wrap the sink explicitly
- sinks must be fast and non-blocking

## Blocking And Joining

The blocking forms consume the object passed to them. Reusing that object after
the call is a bug.

Working shapes:

```cpp
template<class T>
Outcome<T> join(Task<T>&& task);

template<class T>
auto value(Task<T>&& task) -> std::conditional_t<std::is_void_v<T>, void, T>; // throws FailureError / CancelledError

template<class Owner, class T>
Outcome<T> join(Owner& owner, Posted<T>&& posted);

template<class Driver, class T>
Outcome<T> join(Driver& driver, Operation<T>&& op);

template<class Owner, class T>
auto value(Owner& owner, Posted<T>&& posted) -> std::conditional_t<std::is_void_v<T>, void, T>; // throws FailureError / CancelledError

template<class Driver, class T>
auto value(Driver& driver, Operation<T>&& op) -> std::conditional_t<std::is_void_v<T>, void, T>; // throws FailureError / CancelledError

template<class T>
Outcome<T> join(TaskJoinHandle<T>&& h);

template<class T>
auto value(TaskJoinHandle<T>&& h) -> std::conditional_t<std::is_void_v<T>, void, T>; // throws FailureError / CancelledError

template<class Owner, class T>
Outcome<T> join(Owner& owner, PostedJoinHandle<T>&& h);

template<class Owner, class T>
auto value(Owner& owner, PostedJoinHandle<T>&& h) -> std::conditional_t<std::is_void_v<T>, void, T>; // throws FailureError / CancelledError

template<class Driver, class T>
Outcome<T> join(Driver& driver, OperationJoinHandle<T>&& h);

template<class Driver, class T>
auto value(Driver& driver, OperationJoinHandle<T>&& h) -> std::conditional_t<std::is_void_v<T>, void, T>; // throws FailureError / CancelledError
```

All `join(...)` / `value(...)` forms may also throw `JoinContextError` before
terminal completion if the caller does not hold the required progress-driving
capability.

Throwing forms use explicit work exceptions:

- `FailureError`
- `CancelledError`
- `JoinContextError`

`JoinContextError` is for misuse of `join(...)` / `value(...)` from a context
that does not hold the required progress-driving capability.

It is not a terminal outcome and not a `RejectReason`, because the async value
already exists.

The exact class layout is not locked yet, but the categories are locked.

### What `Owner&` And `Driver&` Mean

These parameters are not identity tags.

They are progress-driving capabilities.

Passing one means:

- the caller is allowed to drive this owner/driver
- `join(...)` / `value(...)` may pump it until the awaited value is terminal
- there is no hidden helper thread doing that work elsewhere

If the caller does not hold a valid progress-driving capability,
`join(...)` / `value(...)` throw `JoinContextError`.

That is the public contract for owner-driven categories.

### `Posted<T>`

`join(owner, posted)` pumps the posted-work owner until `posted` reaches a
terminal state.

If the owner type is thread-bound, calling from the wrong thread or without the
required owner token throws `JoinContextError`.

### `Operation<T>`

`join(driver, op)` pumps the operation's subsystem driver until `op` reaches a
terminal state.

The driver capability passed to `join(driver, op)` must cover all progress
required to publish terminal completion for `op`.

The raw progress engine may or may not be the same object that defines the
published completion affinity.

That distinction is part of the public contract:

- the join driver capability supplies all required progress
- the operation's published completion context supplies resumption affinity

If raw driver progress and published completion affinity differ, the subsystem
must still make `join(driver, op)` sufficient.

That means the subsystem must either:

- expose a composite driver capability that pumps both raw progress and
  terminal publication, or
- ensure its own driver pump performs any necessary marshal-back before
  terminal commit

`join(driver, op)` must not rely on an unrelated hidden runtime continuing to
run elsewhere.

## Affinity

Affinity is semantic, but this proposal intentionally locks less syntax than
the previous revised draft.

### What Is Locked

For the root categories:

- `Task<T>` resumes on the context that commits terminal completion unless an
  explicit hop changes that
- `Posted<T>` resumes on its owner
- `Operation<T>` resumes on its published completion context

Affinity is therefore fixed by category in the root model.

### What Is Deferred

The previous revised draft over-specified `pinned_owner` and
`migratable(domain)` without showing the admission or hop surface that carries
those policies.

This proposal moves that back to the deferred carrier layer.

Locked requirement for the later carrier design:

- if a coroutine/carrier can migrate, the allowed resume domain must be
  explicit
- if a hop changes completion/resume context, that hop must be explicit in the
  public API
- no affinity behavior may depend on inferred coroutine-frame allocation shape

Working placeholder names:

- `hop_to(target, source)`
- `resume_on(target)`

Those names are illustrative only. The exact surface remains part of the
deferred carrier decision.

## Cancellation

Cancellation is explicit and best-effort.

### Request Side

`request_cancel(reason)` means:

1. atomically record the first cancel request if none was recorded already
2. arrange category-specific producer notification if supported
3. do not by itself commit a terminal `Cancelled`

The first request reason wins on the request side.

### Terminal Side

Terminal outcome uses first terminal commit wins:

- success may win after a cancel request if the work completes first
- failure may win after a cancel request if the work fails first
- cancelled wins only when some producer/owner/driver path commits cancelled
  before any success/failure commit

`Cancelled.reason` is the reason committed by the winning terminal cancel path.

That means:

- it is not merely "some request happened"
- it may reflect a recorded `requested` / `shutdown`
- it may reflect a subsystem-originated `external`

### Cooperative Cancellation For Running Work

For running `Task<T>` and `Posted<T>` user code, cooperative cancellation needs
an explicit surface.

Working shape:

```cpp
class CancelToken {
public:
    bool stop_requested() const noexcept;
    std::optional<CancelReason> reason() const noexcept;
};
```

Admission and spawn may pass that token to callables that opt in:

```cpp
submit(exec, fn); // where fn is invocable either as fn() or fn(CancelToken)
post(owner, fn);  // where fn is invocable either as fn() or fn(CancelToken)
spawn(exec, fn);  // same opt-in rule
spawn(owner, fn); // same opt-in rule
```

If a callable does not accept a token, cancellation is still meaningful before
start but the running body has no runtime-provided cooperative poll point.

### Category Semantics

For `Task<T>`:

- before start, the executor may skip the task and commit cancelled
- once running, cancellation is cooperative through `CancelToken`

For `Posted<T>`:

- before start, the owner may discard cancelled posted work without running the
  user closure
- once running, cancellation is cooperative through `CancelToken`
- foreign-thread cancellation never performs queue surgery

For `Operation<T>`:

- cancellation may trigger subsystem-specific cancel hooks
- those hooks may attempt kernel, remote, or state-machine cancellation
- terminal cancellation can still require further driver progress

### Explicitly Deferred Cancellation Sources

This proposal intentionally does not define:

- deadline-triggered cancellation
- parent/scope-triggered cancellation

Those reasons and their APIs should be added only when the actual primitives
exist.

## Aggregates And Combinators

The previous revised draft claimed more surface than it actually specified.

This proposal narrows the claim.

### Locked Aggregate Semantics

Only this point is locked now:

- `when_all(...)` uses wait-all semantics by default

That means:

- no implicit fail-fast sibling cancellation in the default form
- no generic assumption that `request_cancel()` promptly drains siblings

### What Is Deferred

This document does not finalize:

- aggregate API names
- aggregate continuation affinity
- `race(...)`
- loser ownership mechanics for early-exit aggregates
- combinator callback placement rules beyond the root-category affinity model

Those belong to the deferred carrier/combinator design, not the root async
model.

## Control Blocks And Reclamation

Each accepted root async boundary owns one intrusive control block.

The control block has:

- common header state
- outcome storage
- request-cancel state
- intrusive refcount
- category-specific progress / waiter state

Refs may be held by:

- the root result object or background join handle
- copyable control handles
- the producer-side source
- deferred combinator/carrier state once that layer is designed

Reclamation rule:

- terminal outcome storage remains alive until the last holder drops its ref

This proposal intentionally does not lock the exact in-memory layout beyond
"common header plus category-specific state" and "one control block per root
boundary".

## Subsystem Mapping

### `WorkPool`

Accepted work becomes `Task<T>`.

Rejection covers:

- `stopped`
- `queue_full`

### `RingLane`

Accepted posted work becomes `Posted<T>`.

Rejection covers:

- `stopped`
- `queue_full`
- `notify_failed`

`notify_failed` still means no work was published.

### `file_io`

Accepted file operations become `Operation<T>`.

The subsystem's driver object pumps CQE progress.

If file I/O publishes owner-affine completion, the file-I/O layer is
responsible for marshalling before terminal commit.

### `db`

Accepted DB query/poll operations become `Operation<T>`.

The DB driver pumps libpq state transitions.

If DB completion is defined as owner-affine, DB is responsible for marshalling
before terminal commit.

### `process`

`process` remains autonomous scheduled work.

Recommended shapes remain payload-oriented when that matches the subsystem best:

- `Task<expected<Process, error_code>>`
- `Task<expected<RunResult, error_code>>`

General rule:

- admission/runtime inability to start is rejection
- post-admission domain results stay in the payload when the subsystem's
  primary product is a domain result rather than "throw on domain failure"

## Explicitly Deferred

Deferred to the later carrier / subsystem design:

- final coroutine return/combinator carrier
- explicit hop primitive spelling
- migratable-domain coroutine policy
- timeout / deadline primitives
- structured-concurrency scopes
- aggregate placement rules
- `race(...)` and fail-fast aggregate ownership mechanics
- droppable / coalescing stream primitives
- optional timing instrumentation and per-spawn budgets

## Why This Version Is Narrower

The previous revised draft mixed three things:

- locked root-category semantics
- plausible future coroutine-carrier semantics
- partially specified aggregate/combinator behavior

This proposal keeps the first, softens the second, and defers the third.

That is intentional. The root model should be correct before the carrier model
tries to make it pleasant.
