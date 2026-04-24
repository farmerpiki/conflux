# `conflux.work` API Redesign (Proposal V5)

Status: proposal from v4 plus follow-up review synthesis

This document replaces
[`conflux-work-api-redesign-proposal-v4.md`](./conflux-work-api-redesign-proposal-v4.md)
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

V5 keeps the root model intentionally narrow, but closes the remaining
contract-precision gaps from V4:

- the design is strictly C++26 and may use standard C++26 vocabulary directly
- the root layer is a foundation layer, not the final ergonomic application API
- every admitted root async value must eventually reach a terminal state
- shutdown must drive outstanding admitted work to a terminal outcome before
  the relevant progress capability becomes invalid
- blocking joins use explicit progress-driving capabilities that must be
  identity-checkable and non-reentrant
- cooperative cancellation for user code uses `std::stop_token`
- cooperative user code sees only the stop signal; terminal `CancelReason` is
  decided on the commit path
- terminal cancellation still carries a distinct `CancelReason`
- `Outcome<T>` now has a defined public contract instead of a forward-declared
  placeholder
- `Outcome<T>` accessor preconditions and `visit(...)` visitor rules are now
  explicit
- `work_value<T>` now requires nothrow move so `Outcome<T>` can keep its
  no-valueless guarantee
- producer-side source destruction now has defined fallback completion semantics
- control-block allocation, stop-state ownership, and borrowed PMR lifetime are
  explicit
- progress capability identity is now defined through an explicit
  `capability_id` customization contract
- `join(...)`, `value(...)`, `can_join(...)`, and source factories now apply
  the progress-capability constraint directly where required
- abandonment now has a defined nothrow sink contract, deterministic overload
  selection, and a precise `scoped_abandon::release()` surface
- root objects keep the hard `std::terminate` destructor rule, but the proposal
  now defines a standard scope guard for unwinding-safe abandonment
- the throwing surface now locks the exception hierarchy and the
  `FailureError` rethrow path
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

### Contract-Violation Convention

When this document says "contract violation", it means the caller has broken a
documented precondition.

The normative consequence is:

- unchecked builds have undefined behavior
- checked or audit builds are encouraged to surface the violation through C++26
  contract instrumentation, assertions, or immediate `std::terminate()`

The root spec does not require throwing `std::logic_error` for contract
violations.

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
- every subsystem that admits root async values must keep enough shutdown-time
  bookkeeping to enumerate outstanding control blocks and drive them to a
  terminal outcome before the relevant capability is invalidated; the exact
  registry/helper mechanism is subsystem-internal and not locked by the root
  spec

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
    std::same_as<T, void> ||
    (std::same_as<T, std::remove_cvref_t<T>> &&
     std::movable<T> &&
     std::is_object_v<T> &&
     (!std::is_array_v<T>) &&
     std::is_nothrow_move_constructible_v<T>);

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
    abandoned,
    shutdown,
    external,
};

struct Cancelled {
    CancelReason reason;
};

enum class OutcomeKind : uint8_t {
    success,
    failure,
    cancelled,
};

template<class T>
class Outcome {
public:
    OutcomeKind kind() const noexcept;

    bool is_success() const noexcept;
    bool is_failure() const noexcept;
    bool is_cancelled() const noexcept;

    Success<T>& success() & noexcept;
    Success<T> const& success() const & noexcept;
    Success<T>&& success() && noexcept;

    Failure& failure() & noexcept;
    Failure const& failure() const & noexcept;
    Failure&& failure() && noexcept;

    Cancelled& cancelled() & noexcept;
    Cancelled const& cancelled() const & noexcept;
    Cancelled&& cancelled() && noexcept;

    template<class F>
    auto visit(F&& f) & -> /* R */;

    template<class F>
    auto visit(F&& f) const & -> /* R */;

    template<class F>
    auto visit(F&& f) && -> /* R */;
};

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
- `Success<T>` is a one-member aggregate. Its copy/move/noexcept properties
  follow `T`, and `Success{42}` is valid CTAD for `Success<int>`.
- `Outcome<T>` is a library vocabulary type, not merely a `using` alias to
  `std::variant`. The public contract is "exactly one of the three states" with
  no `valueless_by_exception` escape hatch.
- `Outcome<void>` is valid through the `Success<void>` specialization.
- `Outcome<T>` move/copy support follows `T`. If `T` is move-only, `Outcome<T>`
  is move-only. If `T` is copyable, `Outcome<T>` is copyable.
- `Outcome<T>::success()`, `failure()`, and `cancelled()` have the obvious arm
  preconditions. Violating those preconditions is a contract violation under
  the convention above. Use `kind()` / `is_*()` / `visit(...)` for total
  inspection.
- `Outcome<void>::success()` returns a reference to the empty
  `Success<void>` specialization. That arm is useful for dispatch, not for
  payload extraction.
- `visit(F&&)` is total inspection. For the `&` overload, `F` must be
  invocable with `Success<T>&`, `Failure&`, and `Cancelled&`, and those three
  invocations must have exactly the same return type. The `const&` and `&&`
  overloads propagate cv/ref qualifiers analogously.
- `visit(F&&)` propagates exceptions thrown by the visitor unchanged.
- `visit(F&&)` is conditionally `noexcept` exactly when the corresponding
  visitor invocations for all three arms are `noexcept`.
- Implementations may realize `visit(...)` with `std::visit`-style machinery or
  equivalent internal dispatch, but the public contract is the qualifier-aware
  three-arm visitation rule above.
- `value(...)` is defined in terms of terminal outcome inspection:
  - `Success<T>` returns `success().value`
  - `Success<void>` returns `void`
  - `Failure` throws `FailureError`
  - `Cancelled` throws `CancelledError`
- `T` must be a complete non-reference type at admission time. Non-`void`
  payloads must be nothrow move-constructible so terminal outcome storage can
  preserve the no-valueless contract.
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
- the decayed admitted result type must satisfy `work_value`
- admission templates should diagnose failure of `admitted_work_fn` with a
  message equivalent to: "Callable must be invocable as exactly one of `fn()`
  or `fn(std::stop_token)`; forwarding callables that match both forms must be
  wrapped to disambiguate."

### Admission Options

V5 keeps the explicit extension point for control-block allocation and future
per-admission policy hooks.

```cpp
struct AdmissionOptions {
    std::pmr::memory_resource* control_block_resource = nullptr;
    // reserved for future budget / perf annotations
};

using SubmitOptions = AdmissionOptions;
using PostOptions = AdmissionOptions;
using OperationOptions = AdmissionOptions;
```

`control_block_resource == nullptr` means "use the subsystem default pool or
memory resource."

The memory resource pointer is borrowed, not owned. It must outlive the final
holder of the control block, including:

- root result objects
- join handles
- copyable control handles
- producer-side sources

Destroying the referenced memory resource before those holders are gone is
undefined behavior.

Implementations may offer optional debug diagnostics for this borrowed-lifetime
rule, but V5 does not require shared ownership, wrapper indirection, or a
standard "safe allocator" mode in the root API.

Admission is specified to translate control-block allocation failure into
`std::unexpected(RejectReason::resource_exhausted)`.

That includes failure allocating:

- the control block itself
- any mandatory stop-state storage required by the embedded `std::stop_source`

### Working Shapes

```cpp
template<class Exec, admitted_work_fn Fn>
    requires work_value<admitted_invoke_result_t<Fn>>
auto submit(Exec& exec, Fn&& fn, SubmitOptions opts = {})
    -> std::expected<Task<admitted_invoke_result_t<Fn>>, RejectReason>;

template<progress_capability Owner, admitted_work_fn Fn>
    requires work_value<admitted_invoke_result_t<Fn>>
auto post(Owner& owner, Fn&& fn, PostOptions opts = {})
    -> std::expected<Posted<admitted_invoke_result_t<Fn>>, RejectReason>;

template<class Exec, admitted_work_fn Fn>
    requires work_value<admitted_invoke_result_t<Fn>>
auto submit_background(Exec& exec, Fn&& fn, SubmitOptions opts = {})
    -> std::expected<TaskJoinHandle<admitted_invoke_result_t<Fn>>, RejectReason>;

template<progress_capability Owner, admitted_work_fn Fn>
    requires work_value<admitted_invoke_result_t<Fn>>
auto post_background(Owner& owner, Fn&& fn, PostOptions opts = {})
    -> std::expected<PostedJoinHandle<admitted_invoke_result_t<Fn>>, RejectReason>;
```

`Exec&` used by `submit(...)` / `submit_background(...)` is not required to
satisfy `progress_capability`. Autonomous task execution does not use
capability-identity checks.

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

template<work_value T>
std::pair<Task<T>, TaskSource<T>> make_task_source(SubmitOptions opts = {});

template<progress_capability Owner, work_value T>
std::pair<Posted<T>, PostedSource<T>> make_posted_source(Owner& owner, PostOptions opts = {});

template<progress_capability Driver, work_value T>
std::pair<Operation<T>, OperationSource<T>> make_operation_source(Driver& driver, OperationOptions opts = {});
```

Producer-side rules:

- first terminal commit wins
- later terminal commits are ignored and return `false`
- `request_cancel()` is not itself terminal completion
- `TaskSource<T>`, `PostedSource<T>`, and `OperationSource<T>` are move-only
- a moved-from source has no commit authority; all commit operations and
  `install_cancel_hook(...)` return `false`
- source objects are single-owner producer handles; concurrent unsynchronized
  `commit_*`, `install_cancel_hook(...)`, or destruction on the same source
  object is a contract violation
- `make_posted_source(...)` and `make_operation_source(...)` bind the required
  owner/driver identity at construction time and store only the copied
  capability identity in the control block, not a borrowed owner/driver pointer
- producer code may commit `Cancelled{requested}`, `Cancelled{shutdown}`, or
  `Cancelled{external}` only through a real subsystem/runtime path that owns
  that reason
- implicit source-destruction fallback authors `Cancelled{abandoned}`
- `CancelReason::abandoned` is reserved for that implicit producer-abandonment
  fallback path
- an explicit call to `commit_cancelled(CancelReason::abandoned)` is a contract
  violation; only the source-destructor fallback may author that reason

### Source Destruction Rule

Destroying a live `TaskSource<T>`, `PostedSource<T>`, or `OperationSource<T>`
without a terminal commit does not strand the paired root value.

If the source still owns commit authority when its destructor runs, the
destructor performs a non-throwing fallback terminal commit as
`Cancelled{CancelReason::abandoned}`.

Source destructors are `noexcept`.

That rule preserves the liveness invariant without requiring producer-side
destructors to call `std::terminate`.

The fallback commit path is required to be allocation-free and wait-free against
any concurrently-running cancel-hook invocation.

During subsystem shutdown, a subsystem-authored
`Cancelled{CancelReason::shutdown}` commit and a source-destructor-authored
`Cancelled{CancelReason::abandoned}` commit may race. The observed terminal
reason is whichever terminal commit wins first.

### Cancel-Hook Contract

Cancel hooks are advisory and category-specific, but the concurrency contract is
now explicit:

- at most one cancel hook may ever be installed for a given source
- a second installation attempt always returns `false`
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
- cancel-hook invocation has no happens-before relationship with state written
  by a racing terminal commit; hook authors must not depend on observing or not
  observing commit-written state

The hook object and its captures live with hook storage retained by the control
block until any in-flight invocation returns.

This means subsystem authors must not capture raw pointers or references to
producer-owned mutable state unless that state is kept alive independently of
terminal-outcome storage, for example through shared ownership or
subsystem-managed kept-alive storage.

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

Root result object destructors are `noexcept`.

That behavior remains a hard contract in V5. The root layer does not
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

Join-handle destructors are `noexcept`.

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

The V2/V3 `background(...)` conversion spelling is retired. V5 uses only
`into_join_handle(...)` for this ownership conversion.

### Explicit Abandon

If a caller truly wants no later `T`, it must say so explicitly:

```cpp
struct drop_on_abandon {
    void operator()(Failure const&) const noexcept {}
    void operator()(Cancelled const&) const noexcept {}
};

template<class Sink, class T>
concept abandon_sink =
    std::is_nothrow_move_constructible_v<Sink> &&
    (std::is_nothrow_invocable_v<Sink&, Outcome<T> const&> ||
     std::is_nothrow_invocable_v<Sink&, Failure const&>);

template<class T, class Sink>
    requires abandon_sink<Sink, T>
void abandon_to(Task<T>&&, Sink&& sink) noexcept;

template<class T, class Sink>
    requires abandon_sink<Sink, T>
void abandon_to(Posted<T>&&, Sink&& sink) noexcept;

template<class T, class Sink>
    requires abandon_sink<Sink, T>
void abandon_to(Operation<T>&&, Sink&& sink) noexcept;

template<class T, class Sink>
    requires abandon_sink<Sink, T>
void abandon_to(TaskJoinHandle<T>&&, Sink&& sink) noexcept;

template<class T, class Sink>
    requires abandon_sink<Sink, T>
void abandon_to(PostedJoinHandle<T>&&, Sink&& sink) noexcept;

template<class T, class Sink>
    requires abandon_sink<Sink, T>
void abandon_to(OperationJoinHandle<T>&&, Sink&& sink) noexcept;
```

`abandon_to(...)` keeps running work alive and has these semantics:

- `Sink` must be nothrow move-constructible
- `Success<T>` is discarded and is never delivered to the sink
- if `Sink` is nothrow-invocable as `Sink&(Outcome<T> const&)`, then failure
  and cancelled outcomes are delivered through that full-outcome overload
- otherwise the sink must be nothrow-invocable as `Sink&(Failure const&)`
- when the full-outcome overload is not used, `Cancelled` is delivered only if
  `Sink` is also nothrow-invocable as `Sink&(Cancelled const&)`
- overload resolution is deterministic: `Outcome<T> const&` delivery wins over
  arm-specific delivery when both are viable
- `drop_on_abandon{}` is the canonical no-op error-path cleanup sink
- the sink runs on the context that commits the terminal outcome
- `abandon_to(...)` does not create progress or marshalling
- sinks must be fast, non-blocking, and `noexcept`
- success-payload destruction on the discard path happens inline on the same
  terminal-commit context
- the root layer does not offload or defer success-payload destruction to a
  background worker
- on the success-discard path the payload is destroyed inline and no sink is
  invoked
- for `Operation<T>`, both discard-path payload destruction and any sink
  invocation run on the subsystem's published completion context; user sinks
  must be appropriate for that context
- `work_value<T>` does not encode destruction cost; subsystems with tighter
  latency budgets may impose stricter local payload guidance, but the root
  layer itself keeps destruction inline

That means payload types used with abandonment must have destruction cost that
is acceptable on the category's commit context, or they must be wrapped in an
indirection/offload strategy outside the root layer.

If a sink violates the non-throwing contract, the implementation catches that
violation and calls `std::terminate`.

### Scope Guard For Unwinding

The destructor-terminates rule is intentionally strict, so the root layer also
defines a standard unwinding-safe abandonment guard:

```cpp
template<class R, class Sink = drop_on_abandon>
class scoped_abandon {
public:
    scoped_abandon(scoped_abandon&&) noexcept = default;
    scoped_abandon& operator=(scoped_abandon&&) = delete;

    ~scoped_abandon() noexcept;

    bool armed() const noexcept;
    R release() && noexcept;
};

template<class R, class Sink = drop_on_abandon>
auto guard_abandon(R&& result, Sink sink = {})
    -> scoped_abandon<std::remove_cvref_t<R>, Sink>;
```

`scoped_abandon<...>` is move-only, nothrow-move-constructible, and
`noexcept`-destructible.

It participates in overload resolution only when `Sink` satisfies the
corresponding `abandon_to(...)` sink contract for the wrapped result type.

If still armed at destruction, it performs:

- `abandon_to(std::move(result), std::move(sink))`

The guard's `release() && noexcept` member transfers the wrapped root object or
join handle back to the caller and disarms the destructor path. It returns the
wrapped object by move even if that object is already terminal.

A moved-from or released guard is empty and its destructor is a no-op.

This is the standard RAII tool for exception-safe root-object cleanup on stack
unwinding.

V5 intentionally does not define a destructor-joining scope guard in the root
layer, because join may require an explicit progress-driving capability and may
block indefinitely.

## Copyable Control Handles

Copyable control handles are observation/cancellation handles only.

Working shapes:

- `TaskControl`
- `PostedControl`
- `OperationControl`

Those handles are intentionally type-erased with respect to `T`.

Every root result object and every background join handle exposes
`control() noexcept` to obtain the corresponding copyable control handle.
`control()` is required to be allocation-free and to do no more than retain one
additional control-block reference.

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
- `stop_token()`
- `cancel_requested()`
- `ready()`
- `state()`
- category tag / minimal state snapshot

They do not surface the success value.

`stop_token()` returns a passive `std::stop_token` view backed by the control
block's embedded `std::stop_source`. That token is the root-layer interop point
for standard stop-token-aware code; V5 does not add a separate
`get_stop_token(root_object)` free function.

### Thread-Safety Contract

Copyable control handles are thread-safe.

At minimum:

- `request_cancel()` is idempotent and safe to call concurrently with itself
  and with terminal commit
- the first successful `request_cancel()` records exactly one request-side
  reason, `CancelReason::requested`
- later `request_cancel()` calls do not overwrite that recorded request-side
  reason
- `ready()`, `cancel_requested()`, and `state()` are acquire snapshots
- a successful first `request_cancel()` publishes the stop request and the
  recorded generic cancel reason with release semantics

### `WorkState` Transitions

The public transition graph is:

```text
pending
  |
  +--> cancel_requested
  |       |
  |       +--> ready_success
  |       +--> ready_failure
  |       +--> ready_cancelled
  |
  +--> ready_success
  +--> ready_failure
  +--> ready_cancelled
```

Rules:

- `cancel_requested` is a non-terminal snapshot
- `cancel_requested` may still resolve as success or failure because
  cancellation is best-effort
- once any `ready_*` state is observed, later observations return that same
  `ready_*` state

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

template<progress_capability Owner, class T>
Outcome<T> join(Owner& owner, Posted<T>&& posted);

template<progress_capability Driver, class T>
Outcome<T> join(Driver& driver, Operation<T>&& op);

template<progress_capability Owner, class T>
auto value(Owner& owner, Posted<T>&& posted)
    -> std::conditional_t<std::is_void_v<T>, void, T>;

template<progress_capability Driver, class T>
auto value(Driver& driver, Operation<T>&& op)
    -> std::conditional_t<std::is_void_v<T>, void, T>;

template<class T>
Outcome<T> join(TaskJoinHandle<T>&& h);

template<class T>
auto value(TaskJoinHandle<T>&& h) -> std::conditional_t<std::is_void_v<T>, void, T>;

template<progress_capability Owner, class T>
Outcome<T> join(Owner& owner, PostedJoinHandle<T>&& h);

template<progress_capability Owner, class T>
auto value(Owner& owner, PostedJoinHandle<T>&& h)
    -> std::conditional_t<std::is_void_v<T>, void, T>;

template<progress_capability Driver, class T>
Outcome<T> join(Driver& driver, OperationJoinHandle<T>&& h);

template<progress_capability Driver, class T>
auto value(Driver& driver, OperationJoinHandle<T>&& h)
    -> std::conditional_t<std::is_void_v<T>, void, T>;
```

Throwing forms use explicit work exceptions:

- `WorkError`
- `FailureError`
- `CancelledError`
- `JoinContextError`

Working shape:

```cpp
class WorkError : public std::runtime_error;

class FailureError : public WorkError {
public:
    std::exception_ptr cause() const noexcept;
    [[noreturn]] void rethrow_cause() const;
};

class CancelledError : public WorkError {
public:
    CancelReason reason() const noexcept;
};

enum class JoinContextReason : uint8_t {
    capability_mismatch,
    thread_precondition,
    reentrant_pump,
};

class JoinContextError : public std::logic_error {
public:
    JoinContextReason reason() const noexcept;
};
```

`FailureError` and `CancelledError` represent terminal async outcomes.
`JoinContextError` represents misuse of the join/value context and remains a
logic error rather than an async outcome.

`value(...)` throws `FailureError` or `CancelledError` for both `void` and
non-`void` payloads.

`value(...)` on `Failure` throws `FailureError`; it does not transparently
rethrow the original exception. Callers that need the original typed exception
recover it via `cause()` / `rethrow_cause()`.

The textual `what()` strings of these exception types are diagnostic only and
are not part of the semantic contract.

If the awaited value is already terminal at the time of the call, `join(...)`
and `value(...)` return immediately without entering an unnecessary pump path.

### `Owner&` And `Driver&`

These parameters are progress-driving capabilities.

Passing one means:

- the caller is allowed to drive this owner/driver
- `join(...)` / `value(...)` may pump it until the awaited value is terminal
- there is no hidden helper thread doing that work elsewhere

Working capability shape:

```cpp
struct capability_id_t {
    template<class Cap>
    auto operator()(Cap const& cap) const noexcept(/* see below */)
        -> decltype(tag_invoke(*this, cap));
};

inline constexpr capability_id_t capability_id{};

template<class Cap>
concept progress_capability =
    requires(Cap const& cap) {
        { capability_id(cap) } noexcept -> std::copyable;
        requires std::equality_comparable<decltype(capability_id(cap))>;
    };
```

`capability_id` is a root-layer customization point object. Public
customization is through that CPO, not through unrelated plain-ADL helper
functions.

The capability-id contract is:

- repeated calls on the same capability object return equal values
- capability wrappers that authorize the same progress domain may also return
  equal values
- progress capabilities that are not mutually substitutable for
  `join(...)` / `can_join(...)` must not compare equal while their relevant
  lifetimes overlap
- the id type need not be hashable; implementations may use thread-local
  stacks, linear scans, or equivalent bookkeeping for reentrancy tracking

The control block stores the copied capability identity chosen at admission or
producer-source construction time. It does not need to retain the original
owner/driver object pointer.

Any stronger lifetime requirement on the original owner/driver object belongs
to the admitting subsystem's own publication/shutdown machinery, not to the
root-layer identity check.

The implementation must be able to check capability compatibility cheaply and
deterministically. Wrong-capability use throws `JoinContextError`.

For generic code that wants a non-throwing probe, the root layer also provides:

```cpp
template<progress_capability Owner>
bool can_join(Owner& owner, PostedControl const&) noexcept;

template<progress_capability Driver>
bool can_join(Driver& driver, OperationControl const&) noexcept;
```

`Task<T>` requires no progress capability and therefore has no `can_join(...)`
probe.

`can_join(...)` is required to be:

- `noexcept`
- non-blocking
- allocation-free

It checks only:

- capability identity
- static thread/token preconditions required by that capability
- current-thread same-capability reentrancy

A `true` result from `can_join(...)` guarantees that an immediately-following
`join(...)` / `value(...)` on that same thread will not throw
`JoinContextError`, unless the caller invalidates those preconditions between
the probe and the blocking call.

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

The root layer does not define cross-capability work stealing or hidden
"helping" by unrelated progress capabilities. `join(cap, ...)` pumps the domain
authorized by `cap` only.

### Reentrancy And Deadlock Boundaries

Same-capability non-reentrancy is checked on a per-calling-thread basis.

The observable rule is:

- if a thread is already pumping capability `X`, then a nested
  `join(X, ...)` / `value(X, ...)` on that same thread throws
  `JoinContextError`

The implementation may realize that rule with thread-local tracking or an
equivalent mechanism. That tracking must be cleared on both normal return and
exception unwind from the blocking call. Cross-owner and cross-driver deadlocks
are not detected at the root layer.

## Affinity

Affinity is semantic, but V5 still keeps the root syntax narrower than the old
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
2. request stop on the control block's embedded `std::stop_source`
3. arrange category-specific producer notification if supported
4. do not by itself commit terminal `Cancelled`

The first request wins on the request side.

Every control block owns one embedded `std::stop_source`.

That is true whether or not the admitted callable accepts `std::stop_token`.

The shared stop-state used by `std::stop_source` / `std::stop_token` may remain
alive after the control block itself is reclaimed if user-retained tokens or
callbacks still reference it. That is standard stop-token lifetime and is not
tied to control-block lifetime.

### Cooperative Cancellation For Running User Code

For running `Task<T>` and `Posted<T>` user code, the cooperative surface is
`std::stop_token`.

Admission and background-admission forms may pass that token to callables that
opt in through the constrained callable rule.

If a callable does not accept `std::stop_token`, cancellation is still
meaningful before start, but the running body has no runtime-provided polling
surface.

V5 makes one narrowing explicit: cooperative code observes only the stop signal.
It does not get a `CancelReason` query from the root layer. Terminal
`CancelReason` remains a commit-path property, observable from the final
`Outcome<T>` rather than from the running `std::stop_token`.

Callbacks registered through standard `std::stop_callback` on such tokens follow
normal C++ stop-token rules and therefore must be `noexcept`. The root layer
does not guarantee that `request_cancel()` synchronously performs kernel,
remote, or subsystem-specific cancellation just because a stop callback runs.

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
- source-destruction fallback authors `abandoned`
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
to satisfy the standard sender concepts in V5.

That is intentional:

- the root layer carries explicit progress-driving capability requirements that
  the standard sender model does not encode directly
- the root layer also keeps linear lifetime and explicit blocking join
  semantics that are foundational for conflux's execution model

Later layers may provide explicit adapters to `std::execution`, but V5 does not
promise `connect`, `start`, or receiver customization on the root objects
themselves.

## Aggregates And Combinators

The root-layer claim remains narrow.

### Locked Aggregate Semantics

Only this point is locked now:

- the eventual wait-all aggregate uses wait-all semantics by default

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
- subsystems are responsible for keeping enough live-control-block bookkeeping
  to fulfill the shutdown liveness invariant above; V5 does not standardize a
  shared registry helper or base class for that bookkeeping

Future performance hints such as time budgets or heapless-expected annotations
belong on the admission options surface, not in ad hoc overloads.

### Memory Ordering Summary

The root-layer atomic ordering contract is:

- intrusive refcount increments/decrements use acquire-release semantics
- the final decrement performs the acquire step needed before destruction and
  deallocation
- first-request cancel publication uses release semantics on success
- terminal outcome publication and the transition to `ready_*` use release
  semantics
- `ready()`, `cancel_requested()`, `state()`, and terminal join observation use
  acquire semantics
- cancel-hook invocation has no additional happens-before edge with a racing
  terminal commit beyond the disjoint-state rule described earlier

The public contract is this acquire/release behavior, not stronger global
ordering.

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

Any published completion-affinity identity needed for `join(...)` /
`can_join(...)` consistency is stored as subsystem-private state in the control
block.

### `db`

Accepted DB query/poll operations become `Operation<T>`.

The DB driver pumps libpq state transitions.

If DB completion is defined as owner-affine, DB is responsible for marshalling
before terminal commit.

Any published completion-affinity identity needed for `join(...)` /
`can_join(...)` consistency is stored as subsystem-private state in the control
block.

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

## Why V5 Is Tighter

V4 got the category split and the linear lifetime direction right, but it still
left a few surface contracts underspecified.

V5 keeps the same narrow scope and makes the remaining edge contracts explicit:

- liveness and shutdown ordering are explicit
- `Outcome<T>` now has an actual public contract
- accessor preconditions and `visit(...)` semantics now have defined behavior
- callable selection is concept-driven and tied to a concrete payload concept
- payloads are constrained to nothrow move so terminal storage can keep its
  no-valueless guarantee
- `std::stop_token` is the cooperative cancellation surface
- stop-state ownership and request/terminal reason separation are explicit
- control-block allocation has an admission-time extension point
- source destruction now resolves as terminal `Cancelled{abandoned}` instead of
  leaving the root value stranded
- cancel-hook, PMR lifetime, shutdown bookkeeping, and join-context races are
  specified instead of implied
- unwinding-safe abandonment has a standard RAII guard
- progress capability identity and `can_join(...)` are now pinned as real
  contracts instead of soft conventions
- the throwing surface is now concrete enough for migration and implementation
- the relation to `std::execution` is stated instead of deferred by silence
