# `conflux.work` API Redesign (Proposal V3)

Status: proposal from v2 plus independent review synthesis

This document replaces
[`conflux-work-api-redesign-proposal-v2.md`](./conflux-work-api-redesign-proposal-v2.md)
as the next design candidate.

It is intentionally incompatible with the current `conflux.work` surface and
targets C++26 directly. This proposal does not preserve source compatibility,
ABI compatibility, or legacy pre-C++26 vocabulary.

The locked pre-plan decisions remain in
[`conflux-work-api-redesign-decisions.md`](./conflux-work-api-redesign-decisions.md).

## Summary

This redesign keeps the three root execution categories explicit because they
already exist underneath conflux:

- autonomous scheduled work
- explicitly posted owner work
- driver / CQE / state-machine operations

V3 keeps the root model intentionally narrow, but tightens the parts that were
still underspecified in V2:

- the design is strictly C++26 and may use standard C++26 vocabulary directly
- the root layer is a foundation layer, not the final ergonomic application API
- every admitted root async value must eventually reach a terminal state
- shutdown must drive outstanding admitted work to a terminal outcome before
  the relevant progress capability becomes invalid
- blocking joins use explicit progress-driving capabilities that must be
  identity-checkable and non-reentrant
- cooperative cancellation for user code uses `std::stop_token`
- terminal cancellation still carries a distinct `CancelReason`
- control-block allocation and future admission extensions have an explicit
  options hook now, not a hand-wavy "later"
- root result objects are not `std::execution` senders in this layer; any
  sender/receiver interop belongs to an adapter or carrier layer

## Target And Scope

### C++26 Baseline

This proposal assumes an exceptions-enabled C++26 build.

It may rely directly on:

- concepts
- `std::expected`
- `std::stop_token`
- `std::move_only_function`
- `std::pmr::memory_resource`

The redesign is not required to offer fallback spellings for older standards.

### Root Layer Only

This document locks the root async model and the lifecycle model.

It does not claim that `Task<T>`, `Posted<T>`, and `Operation<T>` are the
complete application-facing async API. Real application code is expected to
consume these root types through the later carrier/combinator layer or through
subsystem-provided coroutine adapters.

Direct use of `join(...)` / `value(...)` remains valid, but it is the low-level
consumption surface, not the final ergonomic layer.

### What Is Not Locked Here

This proposal does not lock:

- the final coroutine / combinator carrier shape
- the final sender/receiver adapter surface
- the exact hop primitive names
- timer / deadline APIs
- structured-concurrency APIs
- aggregate API names or continuation placement rules

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
- what shutdown must cancel before releasing subsystem resources

The previous two-way split still hid one real distinction too many.

## Load-Bearing Invariants

### Liveness Invariant

Every admitted root async value must eventually reach exactly one terminal
outcome:

- `Success<T>`
- `Failure`
- `Cancelled`

This is a subsystem/runtime responsibility, not a best-effort wish.

In particular:

- if admission succeeds, the caller must have a path to eventual terminal
  observation
- subsystem shutdown must drive every admitted-but-not-yet-terminal root value
  to a terminal outcome before the relevant owner/driver capability becomes
  invalid
- if shutdown prevents normal completion, the subsystem must commit
  `Cancelled{CancelReason::shutdown}` before releasing resources required by the
  outstanding async values

Without that invariant the linear lifetime contract would be unsound.

### One Control Block Per Root Boundary

Each accepted root async boundary owns one intrusive control block.

That control block remains the single source of truth for:

- terminal outcome storage
- cancellation request state
- producer notification state
- category-specific progress / waiter state
- lifetime management for roots, join handles, control handles, and sources

### Explicit Progress Capability

Owner-driven joins do not take ornamental context parameters. They take
progress-driving capabilities.

The implementation must make capability identity cheaply checkable. At minimum,
the control block must retain enough identity to reject the wrong owner/driver
without guessing.

### Non-Reentrant Blocking Pump

Root-layer blocking joins for `Posted<T>` and `Operation<T>` are non-reentrant
with respect to the same progress-driving capability.

If code already executing under owner/driver `X` attempts to call
`join(X, ...)` or `value(X, ...)` on work that would require pumping `X`
recursively, the call throws `JoinContextError`.

This proposal does not require recursive owner/driver pumping support in the
root layer. Nested same-owner waiting belongs in the deferred carrier/coroutine
layer.

## Outcome Model

Admission failure is synchronous and no async value exists yet.

Accepted work can terminate only as success, failure, or cancelled.

```cpp
namespace conflux::work {

template<class T>
concept work_value =
    std::same_as<T, std::remove_cvref_t<T>> &&
    std::movable<T> &&
    std::is_object_v<T> &&
    (!std::is_array_v<T>);

template<class T>
struct Success {
    T value;
};

template<>
struct Success<void> {};

struct Failure {
    std::exception_ptr error;
};

enum class CancelReason {
    requested,
    shutdown,
    external,
};

struct Cancelled {
    CancelReason reason;
};

template<class T>
class Outcome; // exactly one of Success<T>, Failure, Cancelled; no valueless state

enum class RejectReason {
    stopped,
    queue_full,
    owner_violation,
    resource_exhausted,
    notify_failed,
};

} // namespace conflux::work
```

Notes:

- `Success<T>` stays distinct, including for non-`void` payloads. The wrapper
  prevents the success arm from collapsing into payload types that already use
  `expected`, `variant`, or similar sum types.
- `Outcome<T>` is a library vocabulary type, not merely a `using` alias to
  `std::variant`. The public contract is "exactly one of the three states" with
  no `valueless_by_exception` escape hatch.
- `T` must be a complete non-reference object type at admission time. `void` is
  supported only through the `Success<void>` specialization.
- `submit_result_t<Fn>`, `post_result_t<Fn>`, and subsystem equivalents decay
  the selected callable result as `std::remove_cvref_t<...>`. Reference and
  array result types are rejected.
- If user code returns `std::expected<U, E>`, then
  `Success<std::expected<U, E>>` is still success at the root layer. Throwing
  still becomes `Failure`.
- If user code throws, the runtime commits `Failure{std::current_exception()}`
  and the exception does not escape the executor or owner/driver pump.

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

## Admission APIs

Admission is explicit and concept-constrained.

### Callable Form Selection

For `submit(...)`, `post(...)`, `submit_background(...)`, and
`post_background(...)`, user callables may opt into cooperative cancellation in
exactly one of two forms:

```cpp
template<class Fn>
concept stop_token_invocable =
    std::invocable<Fn, std::stop_token> &&
    (!std::invocable<Fn>);

template<class Fn>
concept nullary_work_invocable =
    std::invocable<Fn> &&
    (!std::invocable<Fn, std::stop_token>);

template<class Fn>
concept admitted_work_fn =
    stop_token_invocable<Fn> || nullary_work_invocable<Fn>;

template<class Fn>
using admitted_invoke_result_t =
    std::remove_cvref_t</* result of the unique accepted invocation form */>;
```

Rules:

- exactly one invocation form must be valid
- a callable matching both forms is ill-formed
- generic forwarding callables that match both forms must be wrapped
  explicitly to disambiguate
- move-only callables are supported
- immovable callables are not required to be supported

### Admission Options

V3 adds an explicit extension point for control-block allocation and future
per-admission policy hooks.

```cpp
struct AdmissionOptions {
    std::pmr::memory_resource* control_block_resource = nullptr;
    // reserved for future budget / perf annotations
};

using SubmitOptions = AdmissionOptions;
using PostOptions = AdmissionOptions;
```

`control_block_resource == nullptr` means "use the subsystem default pool or
memory resource."

### Working Shapes

```cpp
template<class Exec, admitted_work_fn Fn>
auto submit(Exec& exec, Fn&& fn, SubmitOptions opts = {})
    -> std::expected<Task<admitted_invoke_result_t<Fn>>, RejectReason>;

template<class Owner, admitted_work_fn Fn>
auto post(Owner& owner, Fn&& fn, PostOptions opts = {})
    -> std::expected<Posted<admitted_invoke_result_t<Fn>>, RejectReason>;

template<class Exec, admitted_work_fn Fn>
auto submit_background(Exec& exec, Fn&& fn, SubmitOptions opts = {})
    -> std::expected<TaskJoinHandle<admitted_invoke_result_t<Fn>>, RejectReason>;

template<class Owner, admitted_work_fn Fn>
auto post_background(Owner& owner, Fn&& fn, PostOptions opts = {})
    -> std::expected<PostedJoinHandle<admitted_invoke_result_t<Fn>>, RejectReason>;
```

Subsystem-owned operations follow the same admission boundary rule:

- if the subsystem cannot arm, register, or publish the operation yet, that is
  synchronous `RejectReason`
- once the operation exists, later problems are `Failure` or `Cancelled`
- subsystem APIs may publish either root result objects or background join
  handles, but the verb is subsystem-specific by design

Examples:

- `file_io` SQE/resource exhaustion before publication is rejection
- `db` refusal before a query operation is armed is rejection
- a kernel, SQL, protocol, or remote error after admission is failure

## Producer-Side Construction

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
std::pair<Task<T>, TaskSource<T>> make_task_source(SubmitOptions opts = {});

template<class T>
std::pair<Posted<T>, PostedSource<T>> make_posted_source(PostOptions opts = {});

template<class T>
std::pair<Operation<T>, OperationSource<T>> make_operation_source(AdmissionOptions opts = {});
```

Producer-side rules:

- first terminal commit wins
- later terminal commits are ignored and return `false`
- `request_cancel()` is not itself terminal completion
- producer code may commit `Cancelled{requested}`, `Cancelled{shutdown}`, or
  `Cancelled{external}` only through a real subsystem/runtime path that owns
  that reason

### Cancel-Hook Contract

Cancel hooks are advisory and category-specific, but the concurrency contract is
now explicit:

- at most one cancel hook may be installed
- installing after terminal completion returns `false`
- if cancellation was already requested and no terminal outcome has yet been
  committed, `install_cancel_hook(...)` invokes the hook synchronously before
  returning `true`
- the hook is invoked at most once
- the hook is never invoked concurrently with itself
- the hook is never invoked while an internal control-block lock is held
- the control block and hook storage remain alive until an in-flight hook
  invocation returns
- terminal commit may race with a running hook, but commit and hook execution
  must operate on disjoint runtime state; commit does not wait for the hook
- no hook invocation begins after terminal completion wins

This means subsystem authors may capture source-owned state in the hook only if
that state is kept alive independently of terminal-outcome storage.

## Lifetime Model

### Root Result Objects

`Task<T>`, `Posted<T>`, and `Operation<T>` are move-only consuming tokens.

A live root result object must be exactly one of:

- joined / awaited
- moved elsewhere
- converted into a typed background join handle
- explicitly abandoned to a sink

Destroying a live root result object calls `std::terminate`, exactly like
destroying a joinable `std::thread`.

This includes exception unwinding.

That behavior remains a hard contract in V3. The root layer does not
auto-detach, auto-join, or silently abandon on destruction.

A moved-from root result object has no associated async value. Destroying a
moved-from root object is a no-op.

### Typed Join Handles For Background Work

Background join handles remain move-only, typed, and join-capable:

```cpp
template<class T>
class TaskJoinHandle;

template<class T>
class PostedJoinHandle;

template<class T>
class OperationJoinHandle;
```

Those join handles:

- are move-only
- are typed by `T`
- can be joined
- can expose a copyable control handle
- do not create progress by themselves

Their destructor also calls `std::terminate` if still live.

Root result objects may be converted into join handles explicitly:

```cpp
template<class T>
TaskJoinHandle<T> into_join_handle(Task<T>&& task) noexcept;

template<class T>
PostedJoinHandle<T> into_join_handle(Posted<T>&& posted) noexcept;

template<class T>
OperationJoinHandle<T> into_join_handle(Operation<T>&& op) noexcept;
```

These conversions change lifetime ownership only. They do not allocate a second
control block and are required to be `noexcept`.

### Explicit Abandon

If a caller truly wants no later `T`, it must say so explicitly:

```cpp
struct drop_on_abandon {
    void operator()(Failure const&) const noexcept {}
    void operator()(Cancelled const&) const noexcept {}
};

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

`abandon_to(...)` keeps running work alive and has these semantics:

- `Success<T>` is discarded
- `Failure` is delivered to the sink
- `Cancelled` is delivered only if the sink accepts it
- `drop_on_abandon{}` is the canonical no-op error-path cleanup sink
- the sink runs on the context that commits the terminal outcome
- `abandon_to(...)` does not create progress or marshalling
- sinks must be fast, non-blocking, and `noexcept`

If a sink violates the non-throwing contract, the implementation may terminate.

## Copyable Control Handles

Copyable control handles are observation/cancellation handles only.

Working shapes:

- `TaskControl`
- `PostedControl`
- `OperationControl`

Those handles are intentionally type-erased with respect to `T`.

Every root result object and every background join handle exposes `control()`
to obtain the corresponding copyable control handle.

They support:

```cpp
enum class WorkState : uint8_t {
    pending,
    cancel_requested,
    ready_success,
    ready_failure,
    ready_cancelled,
};
```

- `request_cancel()`
- `cancel_requested()`
- `ready()`
- `state()`
- category tag / minimal state snapshot

They do not surface the success value.

### Thread-Safety Contract

Copyable control handles are thread-safe.

At minimum:

- `request_cancel()` is idempotent and safe to call concurrently with itself
  and with terminal commit
- `ready()`, `cancel_requested()`, and `state()` are acquire snapshots
- a successful first `request_cancel()` publishes the stop request and the
  recorded generic cancel reason with release semantics

The public contract is acquire/release level synchronization, not blanket
`seq_cst`.

## Blocking And Joining

The blocking forms consume the object passed to them.

Working shapes:

```cpp
template<class T>
Outcome<T> join(Task<T>&& task);

template<class T>
auto value(Task<T>&& task) -> std::conditional_t<std::is_void_v<T>, void, T>;

template<class Owner, class T>
Outcome<T> join(Owner& owner, Posted<T>&& posted);

template<class Driver, class T>
Outcome<T> join(Driver& driver, Operation<T>&& op);

template<class Owner, class T>
auto value(Owner& owner, Posted<T>&& posted)
    -> std::conditional_t<std::is_void_v<T>, void, T>;

template<class Driver, class T>
auto value(Driver& driver, Operation<T>&& op)
    -> std::conditional_t<std::is_void_v<T>, void, T>;

template<class T>
Outcome<T> join(TaskJoinHandle<T>&& h);

template<class T>
auto value(TaskJoinHandle<T>&& h) -> std::conditional_t<std::is_void_v<T>, void, T>;

template<class Owner, class T>
Outcome<T> join(Owner& owner, PostedJoinHandle<T>&& h);

template<class Owner, class T>
auto value(Owner& owner, PostedJoinHandle<T>&& h)
    -> std::conditional_t<std::is_void_v<T>, void, T>;

template<class Driver, class T>
Outcome<T> join(Driver& driver, OperationJoinHandle<T>&& h);

template<class Driver, class T>
auto value(Driver& driver, OperationJoinHandle<T>&& h)
    -> std::conditional_t<std::is_void_v<T>, void, T>;
```

Throwing forms use explicit work exceptions:

- `FailureError`
- `CancelledError`
- `JoinContextError`

`FailureError` and `CancelledError` represent terminal async outcomes.
`JoinContextError` represents misuse of the join/value context and should be
classified as a logic error, not as an async outcome.

`value(...)` throws `FailureError` or `CancelledError` for both `void` and
non-`void` payloads.

### `Owner&` And `Driver&`

These parameters are progress-driving capabilities.

Passing one means:

- the caller is allowed to drive this owner/driver
- `join(...)` / `value(...)` may pump it until the awaited value is terminal
- there is no hidden helper thread doing that work elsewhere

The implementation must be able to check capability compatibility cheaply and
deterministically. Wrong-capability use throws `JoinContextError`.

For generic code that wants a non-throwing probe, the root layer also provides:

```cpp
template<class Owner>
bool can_join(Owner& owner, PostedControl const&) noexcept;

template<class Driver>
bool can_join(Driver& driver, OperationControl const&) noexcept;
```

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

If raw driver progress and published completion affinity differ, the subsystem
must still make `join(driver, op)` sufficient. It must not rely on an unrelated
hidden runtime continuing to run elsewhere.

## Affinity

Affinity is semantic, but V3 still keeps the root syntax narrower than the old
replacement draft.

### What Is Locked

For the root categories:

- `Task<T>` resumes on the context that commits terminal completion unless an
  explicit later-layer hop changes that
- `Posted<T>` resumes on its owner
- `Operation<T>` resumes on its published completion context

Affinity is therefore fixed by category in the root model.

### What Is Deferred

The carrier layer may add explicit hops or migratable policies later, but it
must preserve two rules:

- a hop that changes resume/completion context must be explicit in the public
  API
- it must not silently invalidate the progress-driving capability contract of
  an already-admitted root async value

Working placeholder names remain:

- `hop_to(target, source)`
- `resume_on(target)`

Those names are illustrative only.

## Cancellation

Cancellation is explicit and best-effort.

### Request Side

`request_cancel()` means:

1. atomically record the first generic cancel request as
   `CancelReason::requested` if none was recorded already
2. request stop on the control block's internal `std::stop_source`
3. arrange category-specific producer notification if supported
4. do not by itself commit terminal `Cancelled`

The first request wins on the request side.

### Cooperative Cancellation For Running User Code

For running `Task<T>` and `Posted<T>` user code, the cooperative surface is
`std::stop_token`.

Admission and background-admission forms may pass that token to callables that
opt in through the constrained callable rule.

If a callable does not accept `std::stop_token`, cancellation is still
meaningful before start, but the running body has no runtime-provided polling
surface.

### Terminal Side

Terminal outcome uses first terminal commit wins:

- success may win after a cancel request if the work completes first
- failure may win after a cancel request if the work fails first
- cancelled wins only when some producer/owner/driver path commits cancelled
  before any success/failure commit

`Cancelled.reason` is the reason committed by the winning terminal cancel path.

That means:

- it is not merely "some request happened"
- a user `request_cancel()` can only author `requested`
- subsystem shutdown authors `shutdown`
- subsystem-originated external interruption authors `external`

### Category Semantics

For `Task<T>`:

- before start, the executor may skip the task and commit cancelled
- once running, cancellation is cooperative through `std::stop_token`

For `Posted<T>`:

- before start, the owner may discard cancelled posted work without running the
  user closure
- once running, cancellation is cooperative through `std::stop_token`
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

## Relationship To `std::execution`

This root layer is not itself a sender/receiver surface.

`Task<T>`, `Posted<T>`, `Operation<T>`, and their join handles are not specified
to satisfy the standard sender concepts in V3.

That is intentional:

- the root layer carries explicit progress-driving capability requirements that
  the standard sender model does not encode directly
- the root layer also keeps linear lifetime and explicit blocking join
  semantics that are foundational for conflux's execution model

Later layers may provide explicit adapters to `std::execution`, but V3 does not
promise `connect`, `start`, or receiver customization on the root objects
themselves.

## Aggregates And Combinators

The root-layer claim remains narrow.

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

## Control Blocks And Allocation

Each accepted root async boundary owns one intrusive control block.

The control block has:

- common header state
- outcome storage
- request-cancel and stop-state storage
- intrusive refcount
- category-specific progress / waiter state

Refs may be held by:

- the root result object or background join handle
- copyable control handles
- the producer-side source
- deferred combinator/carrier state once that layer is designed

Reclamation rule:

- terminal outcome storage remains alive until the last holder drops its ref

Allocation rule:

- control-block storage comes either from the caller-provided
  `control_block_resource` or from the subsystem default pool/resource
- the root model does not require global `operator new` as the only allocation
  path

Future performance hints such as time budgets or heapless-expected annotations
belong on the admission options surface, not in ad hoc overloads.

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

- `Task<std::expected<Process, std::error_code>>`
- `Task<std::expected<RunResult, std::error_code>>`

General rule:

- admission/runtime inability to start is rejection
- post-admission domain results stay in the payload when the subsystem's
  primary product is a domain result rather than "throw on domain failure"

## Explicitly Deferred

Deferred to the later carrier / subsystem design:

- final coroutine return/combinator carrier
- sender/receiver adapters
- explicit hop primitive spelling
- migratable-domain coroutine policy
- timeout / deadline primitives
- structured-concurrency scopes
- aggregate placement rules
- `race(...)` and fail-fast aggregate ownership mechanics
- droppable / coalescing stream primitives
- opt-in timing instrumentation and per-admission budgets
- final exception class layout details

## Why V3 Is Tighter

V2 got the category split and the linear lifetime direction right, but it still
left too many implementation choices hidden in prose.

V3 keeps the same narrow scope and makes the contract implementable:

- liveness and shutdown ordering are explicit
- callable selection is concept-driven
- `std::stop_token` is the cooperative cancellation surface
- control-block allocation has an admission-time extension point
- cancel-hook and join-context races are specified instead of implied
- the relation to `std::execution` is stated instead of deferred by silence
