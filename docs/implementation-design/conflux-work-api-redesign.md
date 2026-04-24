# `conflux.work` API Redesign

Status: replacement draft

This document replaces the previous redesign draft.

It is intentionally incompatible with the current `conflux.work` surface.
The goal is not to preserve naming or source compatibility. The goal is to
preserve conflux's performance model and execution models while making the API
explicit, predictable, and pleasant in both normal code and coroutine code.

## Core Decisions

1. `conflux.work` exposes two first-class async result categories, not one.
2. Rejection is synchronous at the admission boundary. An accepted async value
   is never later "rejected".
3. Every accepted async value owns one intrusive shared control block.
4. Result objects are linear. A live result object must be consumed or
   explicitly detached.
5. Cancellation is explicit and contract-driven.
6. Coroutine resumption affinity is part of the public contract.
7. Owner-driven execution stays explicit. There is no hidden helper runtime.

Those decisions are the entire design. Everything else follows from them.

## Why Two Categories

Conflux has two fundamentally different execution models:

- autonomous work: once admitted, progress happens without the caller driving
  an owner loop
- owner-driven work: progress only happens while a specific owner pumps a ring,
  lane, or subsystem driver

Pretending those are the same kind of object produces footguns. The API should
not hide that difference.

The new surface therefore uses:

- `Task<T>` for autonomous work
- `Operation<T>` for owner-driven work

Both are cancellable, composable, and awaitable. They do not have the same
blocking semantics.

## Design Goals

- Preserve the current execution models:
  - worker-pool CPU and blocking work
  - owner-driven ring and single-issuer work
  - external completion sources for `file_io`, `db`, TLS, and protocol code
- Make cancellation and ownership explicit.
- Make coroutine resume affinity explicit.
- Keep the hot path to one control-block allocation per accepted async
  boundary.
- Keep queueing and wakeup policies bounded and observable.
- Support both pipeline style and coroutine style without one being a wrapper
  around the other.

## Non-Goals

- No hidden global event loop.
- No automatic background driver thread for owner-driven operations.
- No rejected terminal state for already-admitted work.
- No attempt to erase domain-specific error models from subsystems like
  `process`.

## Result Categories

### `Task<T>`

`Task<T>` is an accepted autonomous async result.

Use it for:

- `WorkPool` jobs
- background CPU work
- blocking adapters that run on worker threads
- any external source whose completion may happen without an owner pump

`Task<T>` supports:

- `wait(task)`
- `get(task)`
- `co_await task`
- composition with `then`, `and_then`, `on_failure`, `on_cancel`

### `Operation<T>`

`Operation<T>` is an accepted owner-driven async result.

Use it for:

- `RingLane` work
- `file_io`
- `db`
- any result whose completion is driven by a specific owner thread or driver

`Operation<T>` supports:

- `block_on(owner, op)`
- `get(owner, op)`
- `co_await op` inside owner-bound coroutines
- composition with `then`, `and_then`, `on_failure`, `on_cancel`

`Operation<T>` does not support plain `wait(op)` or plain `get(op)`.
That omission is intentional.

## Shared Outcome Model

Accepted async values terminate in exactly one of three states:

- success
- failure
- cancelled

Admission failure is not part of the outcome model because admission failure is
synchronous and no async value exists yet.

```cpp
namespace conflux::work {

struct ok_t {};
inline constexpr ok_t ok{};

enum class CancelReason : uint8_t {
    requested,
    deadline,
    parent,
    shutdown,
    external,
};

struct Failure {
    std::exception_ptr error;
};

struct Cancelled {
    CancelReason reason;
};

struct Reject {
    enum class Reason : uint8_t {
        stopped,
        queue_full,
        owner_violation,
        wake_failed,
    } reason;
};

template<class T>
using Success = std::conditional_t<std::is_void_v<T>, ok_t, T>;

template<class T>
using Outcome = std::variant<Success<T>, Failure, Cancelled>;

} // namespace conflux::work
```

### Interpretation

- `Reject` means the work was not admitted.
- `Failure` means the work was admitted and then completed with an error.
- `Cancelled` means the work was admitted and then completed by cancellation.

This is a hard rule.

## Ownership And Control Blocks

Every accepted `Task<T>` and `Operation<T>` owns exactly one intrusive control
block.

That control block carries:

- intrusive reference count
- terminal outcome storage
- cancel-request bit and reason
- one optional producer-side cancel hook
- one optional progress sink
- state needed for result consumption
- category-specific snapshot state

Public result objects stay move-only. The control block is shared internally.
That is the explicit design choice.

There is no claim of "no shared ownership internally". The claim is narrower:

- one intrusive control block allocation per accepted async boundary
- no `std::shared_ptr`
- no additional allocations for moving result objects
- no additional allocations for cloning control handles

## Result Objects And Control Handles

The result object is linear and owns the obligation to consume or detach the
result.

The control handle is copyable and owns cancellation and observation.

```cpp
namespace conflux::work {

template<class T>
class [[nodiscard]] Task {
public:
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    ~Task();

    bool valid() const noexcept;
    TaskHandle handle() const noexcept;
    auto operator co_await() &&;
};

template<class T>
class [[nodiscard]] Operation {
public:
    Operation(Operation&&) noexcept = default;
    Operation& operator=(Operation&&) noexcept = default;
    ~Operation();

    bool valid() const noexcept;
    OperationHandle handle() const noexcept;
    auto operator co_await() &&;
};

class TaskHandle {
public:
    bool valid() const noexcept;
    void request_cancel(CancelReason = CancelReason::requested) noexcept;
    bool cancel_requested() const noexcept;
    TaskSnapshot snapshot() const noexcept;
};

class OperationHandle {
public:
    bool valid() const noexcept;
    void request_cancel(CancelReason = CancelReason::requested) noexcept;
    bool cancel_requested() const noexcept;
    OperationSnapshot snapshot() const noexcept;
};

} // namespace conflux::work
```

### Drop Semantics

A live result object behaves like `std::thread`.

A `Task<T>` or `Operation<T>` must be exactly one of:

- awaited
- consumed via `wait/get` or `block_on/get(owner, ...)`
- explicitly detached
- moved elsewhere

Destroying a still-live result object without doing one of those is a program
error and terminates.

This is deliberate. Conflux has too many long-lived background flows for silent
implicit detach to be safe.

If a caller wants fire-and-forget semantics, it must say so:

```cpp
void detach(Task<T>&&);
void detach(Operation<T>&&);
```

Detached work keeps running. Detached failures go to the detached-error sink.
Detached cancellation is silent.

### Why This Split Exists

The result object is linear because forgetting to consume it is a correctness
problem.

The control handle is copyable because request lifetimes, connection lifetimes,
and shutdown logic need explicit cancellation ownership.

That means server code should typically do this:

- create async result
- extract handle
- store handle in request or connection state
- explicitly `detach(...)` the result if no direct result consumption is needed

## Category-Specific Snapshots

There is no single snapshot shape for both categories.

```cpp
struct TaskSnapshot {
    bool ready = false;
    bool cancel_requested = false;
    bool queued = false;
    bool running = false;
    std::chrono::nanoseconds queued_for{};
    std::chrono::nanoseconds running_for{};
};

struct OperationSnapshot {
    bool ready = false;
    bool cancel_requested = false;
    bool visible_to_owner = false;
    bool waiting_for_owner = false;
    bool running_on_owner = false;
};
```

`TaskSnapshot` is scheduler-shaped because `Task` is scheduler-driven.
`OperationSnapshot` is owner-shaped because `Operation` is owner-driven.

## Producer Types

External completion sources need explicit producer-side objects.

```cpp
template<class T>
class TaskPromise {
public:
    bool valid() const noexcept;
    void set_value(Success<T> value = ok);
    void set_failure(std::exception_ptr error);
    void set_cancelled(CancelReason reason = CancelReason::external);
    bool on_cancel(std::move_only_function<void(CancelReason)> fn);
    void report(ProgressEvent ev) noexcept;
};

template<class T>
class OperationPromise {
public:
    bool valid() const noexcept;
    void set_value(Success<T> value = ok);
    void set_failure(std::exception_ptr error);
    void set_cancelled(CancelReason reason = CancelReason::external);
    bool on_cancel(std::move_only_function<void(CancelReason)> fn);
    void report(ProgressEvent ev) noexcept;
};

template<class T>
std::pair<Task<T>, TaskPromise<T>> make_task(ProgressSink = {});

template<class T>
std::pair<Operation<T>, OperationPromise<T>> make_operation(ProgressSink = {});
```

The split is intentional:

- `TaskPromise<T>` produces `Task<T>`
- `OperationPromise<T>` produces `Operation<T>`

A subsystem must choose which category it is creating.

## Admission APIs

Admission is synchronous and explicit.

```cpp
template<class Exec, class Fn>
auto submit(Exec& exec, Fn&& fn, SubmitOptions opts = {})
    -> std::expected<Task<submit_result_t<Fn>>, Reject>;

template<class Owner, class Fn>
auto post(Owner& owner, Fn&& fn, PostOptions opts = {})
    -> std::expected<Operation<post_result_t<Fn>>, Reject>;
```

### Admission Rule

If `submit(...)` or `post(...)` succeeds, the async value exists and can only
finish with `Outcome<T>`.

If admission fails, no async value exists and the caller gets `Reject`.

There is no rejected `Task<T>` and no rejected `Operation<T>`.

## Blocking Consumption

### Autonomous

```cpp
template<class T>
Outcome<T> wait(Task<T>&& task);

template<class T>
Success<T> get(Task<T>&& task); // throws on failure/cancel
```

### Owner-Driven

```cpp
template<class Owner, class T>
Outcome<T> block_on(Owner& owner, Operation<T>&& op);

template<class Owner, class T>
Success<T> get(Owner& owner, Operation<T>&& op); // throws on failure/cancel
```

`block_on(owner, op)` is a category-level contract, not an optional helper.
Each owner-driven subsystem must provide the correct pumping implementation for
its owner type.

## Composition

The combinator vocabulary is shared. The category is preserved.

```cpp
template<class Fn> auto then(Fn&& fn);       // T -> U
template<class Fn> auto and_then(Fn&& fn);   // T -> Task<U> or Operation<U>
template<class Fn> auto on_failure(Fn&& fn); // Failure -> T or same category
template<class Fn> auto on_cancel(Fn&& fn);  // Cancelled -> T or same category
```

Rules:

- `Task<T> | then(...)` returns `Task<U>`
- `Operation<T> | then(...)` returns `Operation<U>`
- category changes are never implicit

### No Implicit Cross-Category Await Or Composition

The following is intentionally not implicit:

- awaiting a `Task<T>` inside an `Operation<U>` coroutine
- awaiting an `Operation<T>` inside a `Task<U>` coroutine
- flattening `Task<Operation<T>>` or `Operation<Task<T>>`

Cross-category bridges must be explicit.

## Bridges

The one bridge the design commits to is Task-to-Operation marshalling.

```cpp
template<class Owner, class T>
Operation<T> marshal_to(Owner& owner, Task<T>&& task);
```

`marshal_to(owner, task)` means:

- the task executes autonomously
- the resulting completion is forwarded onto the owner's completion path
- an awaiting owner-driven coroutine resumes on that owner, not on the task's
  completion thread

There is deliberately no implicit Operation-to-Task bridge in core work.
If a subsystem needs that, it must define the driver contract explicitly.

## Routing And Affinity

### `Task<T>`

Default rule:

- awaiting a `Task<T>` resumes on the thread or execution context that commits
  the task's terminal outcome

Explicit hop:

```cpp
template<class Exec>
auto transfer_to(Exec& exec);

template<class Exec>
auto resume_on(Exec& exec); // coroutine awaitable for Task coroutines
```

### `Operation<T>`

Default rule:

- awaiting an `Operation<T>` resumes on the owner that commits the operation's
  terminal outcome
- it never resumes on a foreign thread

That is a hard contract.

This is the main reason `Operation<T>` exists as a separate category.

## Cancellation Contract

This section is normative.

### Request Side

`request_cancel(reason)` does exactly this:

1. sets the cancel-request bit if it was not already set
2. stores the first cancel reason
3. arranges producer notification if a cancel hook is installed
4. does not itself produce terminal completion

Cancellation request is idempotent. The first reason wins.

### Producer Hook Contract

`on_cancel(fn)` has this exact behavior:

1. At most one producer-side cancel hook is active.
2. Registering after terminal completion fails and returns `false`.
3. Registering after cancellation has already been requested but before
   terminal completion invokes the hook inline before `on_cancel()` returns,
   then returns `true`.
4. The hook is invoked at most once.
5. The hook is never invoked concurrently with itself.
6. The hook is never invoked while the control-block lock is held.
7. Terminal completion and cancellation race. The first terminal commit wins.
8. If terminal completion wins first, no later cancel hook invocation occurs.
9. If cancellation wins first, later `set_value` / `set_failure` are ignored.

This is the contract `file_io` and `db` need.

### Task Cancellation Semantics

For admitted autonomous work:

- if the work has not started, the executor may skip it and complete cancelled
- if the work is running, cancellation is cooperative via the injected token or
  explicit polling
- there is no hidden thread interruption

### Operation Cancellation Semantics

For admitted owner-driven work:

- foreign-thread cancellation does not perform queue surgery
- the queue node is marked cancelled
- the owner observes that mark at the next safe owner-visible point
- if the operation has not started, the owner completes it as cancelled without
  running user code
- if the operation is already running on the owner, cancellation is cooperative

This is how first-class cancellation and owner affinity coexist without hidden
threads.

### Combinator Propagation Rules

These are the default propagation rules:

- `then` and `and_then`:
  - source success continues
  - source failure propagates unchanged
  - source cancellation propagates unchanged
  - cancelling the derived value requests cancellation of the unfinished source
- `on_failure`:
  - only intercepts failure
  - source success and source cancellation propagate unchanged
- `on_cancel`:
  - only intercepts cancellation
  - source success and source failure propagate unchanged

Aggregation defaults:

- `when_all(...)` is fail-fast:
  - first failure or cancellation wins
  - unfinished siblings receive cancellation request
- `race(...)` completes with the first terminal outcome:
  - unfinished siblings receive cancellation request

These are the only aggregation semantics this draft commits to.
An `all_settled(...)` style API can be added later as a separate primitive.

## Coroutine Contract

There are two coroutine families.

### Task Coroutines

A coroutine returning `Task<T>` may directly `co_await`:

- `Task<U>`
- immediate values and ready adapters
- `resume_on(exec)`

It may not directly `co_await Operation<U>`.

### Operation Coroutines

A coroutine returning `Operation<T>` may directly `co_await`:

- `Operation<U>`
- immediate values and ready adapters
- `marshal_to(owner, task)`

It may not directly `co_await Task<U>` without an explicit bridge.

### Why This Restriction Exists

Implicitly awaiting a task inside an owner-driven coroutine would make resume
thread ambiguous and would create off-owner bugs in `file_io` and `db`.

The API forbids that ambiguity.

## Progress

Progress remains opt-in and minimal.

```cpp
struct ProgressEvent {
    uint32_t code = 0;
    uint64_t current = 0;
    uint64_t total = 0;
};

using ProgressSink = std::move_only_function<void(ProgressEvent const&)>;
```

Contract:

- one sink per async value
- progress reports are delivered synchronously on the reporting context
- progress callbacks for one async value are serialized
- progress callbacks never run concurrently with terminal delivery for that
  same async value
- sinks must be fast and non-blocking
- if a caller wants marshalling to another context, it wraps the sink

There is no borrowed string payload in the core progress type.

## Timers And Deadlines

Core work does not provide a hidden timer runtime.

Timeout support is category-specific and always explicit:

```cpp
template<class Timer, class T>
Task<T> within(Timer& timer, Task<T>&& task, std::chrono::nanoseconds budget);

template<class Timer, class T>
Operation<T> within(Timer& timer, Operation<T>&& op, std::chrono::nanoseconds budget);
```

The timer object must match the category:

- task timer for `Task<T>`
- owner-driven timer for `Operation<T>`

There is no bare `timeout_after(...)` in core work.

## Executor And Owner Contracts

### `WorkPool`

`WorkPool` is an autonomous executor.

Admission may reject with:

- `stopped`
- `queue_full`

Accepted work becomes `Task<T>`.

Cancellation before start may tombstone the queue node.
Running work is cooperatively cancelled.

### `RingLane`

`RingLane` is an owner-driven executor.

Admission may reject with:

- `stopped`
- `queue_full`
- `wake_failed`

Accepted work becomes `Operation<T>`.

Foreign-thread cancellation marks nodes cancelled. The owner discards them on
`drain()` before invoking user code.

## Mapping Rules For Existing Subsystems

### `file_io`

`file_io` returns `Operation<T>`.

It stays owner-driven.
It keeps `block_on(reader, op)`.
It uses `OperationPromise<T>` internally.
It uses the producer-side cancel hook to issue kernel cancellation or stale-CQE
suppression.

`get_sqe() == nullptr` and similar file-I/O submission/resource errors remain
file-I/O failures, not work rejection.

### `db`

`db` returns `Operation<T>`.

It stays owner-driven.
It uses `OperationPromise<T>`.
Its libpq cancel path is driven through the producer-side cancel hook.
Off-owner misuse remains a DB-level failure unless the misuse happens at a
plain `post(owner, ...)` boundary.

### `process`

`process` remains autonomous worker-pool work.

The runtime does not force `process` to turn domain errors into failures or
rejections. The recommended shape is still:

- `Task<expected<Process, error_code>>`
- `Task<expected<RunResult, error_code>>`

Admission rejection only covers worker-pool admission.
`execve`-level failures remain process-domain results.

Cancellation of `wait_in` cancels the waiting async value only. It does not
implicitly signal the child process. Signalling remains explicit via process
APIs.

### `router` / `http_server`

Long-lived request or connection work must store control handles, not result
objects.

Recommended pattern:

1. create `Task<T>` or `Operation<T>`
2. extract `TaskHandle` / `OperationHandle`
3. store handle in request or connection state
4. `detach(...)` the result object if no direct consumption is needed
5. cancel stored handles on disconnect, shutdown, or request teardown

This makes request-lifetime ownership explicit.

## Performance Targets

The design is only acceptable if the implementation keeps these properties:

- one intrusive control-block allocation per accepted async boundary
- move-only result objects
- copyable control handles with intrusive refcount only
- no `std::shared_ptr`
- no `std::function` in executor queues
- bounded queues on autonomous and owner-driven executors
- no hidden helper thread for owner-driven progress
- no foreign-thread queue surgery for owner-driven cancellation
- no dynamic string allocation in the core progress path

## What This Design Does Not Try To Solve

- generic cross-category flattening
- a universal driver abstraction for every owner-driven subsystem
- implicit conversion between autonomous and owner-driven coroutines

Those can be added later if a concrete use case demands them.

The current redesign does not take them on because they are exactly where APIs
usually become vague and slow.
