# `conflux.work` Carrier API Reference

This document describes the current public interface and behavior contract of the
carrier layer (`conflux.work.carrier.*`) as implemented in this repository.

Carrier APIs are advanced/experimental compared with the preferred
`conflux.work.root` vocabulary. New public-facing async code should default to
root types (`Task`, `Posted`, `Operation`) unless it specifically needs carrier
hop/composition behavior.

## Imports

```cpp
import conflux.work.carrier;           // Chain<T>, combinators, hop surface
import conflux.work.carrier.scope;     // Scope, admit
import conflux.work.carrier.deadline;  // DeadlineScope
import conflux.work.carrier.coro;      // EagerChain<T>, async awaiters
import conflux.work.carrier.timer;     // TimerService, LaneTimerScope<>
import conflux.work.carrier.streams;   // DroppableSlot<T>, CoalescingSlot<T>
```

All carrier types live in `conflux::work::carrier`.

## Naming Conventions

| Pattern | Meaning |
|---|---|
| `from_*(root_type) → Chain<T>` | Construct carrier from root type |
| `into_*(Chain<T>) → root_type` | Convert carrier back to root type |
| `hop_to_*(cap, chain)` | Re-bind and/or change `CarrierKind` |
| `admit(jh) → Chain<T>` | Track-then-join via a `Scope` |

Do not invent new patterns outside this table. Naming divergence is a review
blocker.

---

## `Chain<T>`

`Chain<T>` is the carrier's primary async value type. It wraps a completed
`root::Outcome<T>` together with kind and capability metadata.

```cpp
template<root::work_value T>
class Chain {
public:
    Chain() = delete;
    Chain(Chain&&) noexcept = default;
    Chain& operator=(Chain&&) noexcept = default;
    Chain(Chain const&) = delete;
    Chain& operator=(Chain const&) = delete;

    [[nodiscard]] CarrierKind kind() const noexcept;
    [[nodiscard]] root::CapabilityId bound_capability() const noexcept;
    [[nodiscard]] root::Outcome<T> release_outcome() && noexcept;
    [[nodiscard]] ChainAwaiter<T> operator co_await() && noexcept;
};
```

**Ownership:** `Chain<T>` is move-only. Consuming `release_outcome()` or
`operator co_await()` transfers the `Outcome<T>` out. After either call the
`Chain` is moved-from and must not be used again.

**`release_outcome()`** — extracts the completed outcome. This is the escape hatch
for code that needs the raw `Outcome<T>` for custom routing. Prefer combinators
or `co_await` for normal use.

**`operator co_await()`** — for use inside `EagerChain<T>` coroutines only.
Unwraps the outcome: returns the `T` value on success, rethrows the stored
exception on failure, throws `root::CancelledError` on cancellation.

**`CarrierKind`** (`task`, `posted`, `operation`) tracks the category of the
original root source. Hop functions use it for precondition checks.

**`bound_capability()`** — returns the capability bound by the last hop
(`hop_to_posted` / `hop_to_operation`) or zero if unbound. Zero `address` field
means "any capability is accepted" (used by `verify_hop`).

---

## Construction — `from_*`

```cpp
template<work_value T>
Chain<T> from_task(root::Task<T>&&);

template<work_value T, progress_capability Owner>
Chain<T> from_posted(Owner&, root::Posted<T>&&);

template<work_value T, progress_capability Driver>
Chain<T> from_operation(Driver&, root::Operation<T>&&);
```

**Precondition:** the root value must be live (not moved-from, not already
joined). Violating this terminates.

**Semantics:** calls `root::join(...)` synchronously on the calling thread.
`from_task` joins the task inline (blocks until the task reaches a terminal
commit). `from_posted` and `from_operation` additionally validate capability
identity before joining.

**Category:** the resulting `Chain<T>` is unbound (`bound_capability()` is
zero). The `kind()` reflects the input category.

---

## Combinators

All combinators are eager — they operate on already-resolved chains.

### `map` and `then`

```cpp
template<work_value T, class Fn>
auto map(Chain<T>&&, Fn&&) -> Chain<U>;   // U = invoke_result_t<Fn&, T>

template<work_value T, class Fn>
auto then(Chain<T>&&, Fn&&) -> Chain<U>;  // alias for map
```

Applies `fn` to the success value. Failure and cancellation pass through
unchanged. If `fn` throws, the result is a `Failure` wrapping the exception.

Requires `T != void` and `fn(T) -> U` where `U` satisfies `work_value`. The
output `Chain<U>` preserves the input's `kind()`.

### `when_all`

```cpp
template<work_value A, work_value B>
Chain<std::tuple<A, B>> when_all(Chain<A>&&, Chain<B>&&) noexcept;
```

Both arms must succeed. Error-propagation precedence:
1. If both fail: wrap both exception_ptrs in `AggregateError`.
2. If one fails: propagate that failure.
3. If one is cancelled: propagate that cancellation.
4. Both success: return `Chain<tuple<A,B>>`.

Output `kind()` is always `CarrierKind::task`.

### `when_all_fast_fail`

```cpp
template<work_value A, work_value B>
Chain<std::tuple<A, B>> when_all_fast_fail(Chain<A>&&, Chain<B>&&) noexcept;
```

**Known limitation:** currently identical to `when_all`. The name promises
sibling-cancellation on first failure (cancel the still-pending sibling when
the first arm fails), but the eager carrier already has both arms resolved
before this combinator runs. Sibling-cancel semantics require an async carrier
path (Phase 5c) that does not yet exist.

**Consumers MUST NOT** depend on sibling-cancellation behaviour. This combinator
is safe to use; it simply does not yet deliver the promised fast-fail
optimization. The TODO at `carrier.cxx` marks this gap.

### `race`

```cpp
template<work_value T>
Chain<T> race(Chain<T>&&, Chain<T>&&) noexcept;
```

Returns the first arm that succeeded, the first that failed if neither
succeeded, or the first cancellation if both are cancelled. Output `kind()` and
`bound_capability()` are taken from the winning arm.

---

## Hop Surface

```cpp
template<work_value T, progress_capability Owner>
Chain<T> hop_to_posted(Owner&, Chain<T>&&) noexcept;

template<work_value T, progress_capability Driver>
Chain<T> hop_to_operation(Driver&, Chain<T>&&) noexcept;

template<work_value T>
Chain<T> hop_to_task(Chain<T>&&) noexcept;

template<work_value T>
Chain<T> unbind(Chain<T>&&) noexcept;
```

**`hop_to_posted` / `hop_to_operation`** — bind the chain to the given
capability. Sets `kind()` and `bound_capability()` on the resulting chain.
Used before handing the chain to a consumer that must re-join on a specific
owner or driver.

**`hop_to_task`** — clears binding; resulting chain has `kind() == task` and
zero `bound_capability()`.

**`unbind`** — clears the bound capability while preserving `kind()`. Use when
you want to hand the chain to a consumer that will bind it later.

### `verify_hop`

```cpp
template<progress_capability Cap, work_value T>
void verify_hop(Cap const&, Chain<T> const&);
```

Throws `HopCapabilityError` if the chain is bound and its `bound_capability()`
does not match `capability_id(cap)`. A chain with zero `bound_capability()` is
always accepted. Call this at the entry of any function that must enforce
that a chain was prepared for its specific capability.

---

## Output — `into_ready_task`

```cpp
template<work_value T>
root::Task<T> into_ready_task(Chain<T>&&);
```

Converts a completed `Chain<T>` back to a `root::Task<T>` that is already in
a terminal state. The resulting `Task<T>` can be passed to root join functions.

**This is not a hot-path function.** It allocates a control block and
immediately commits the outcome into it. Use only at API boundaries where a
root type is required — do not call `into_ready_task` inside tight loops or
on high-frequency chains.

---

## Error Types

### `HopCapabilityError`

```cpp
class HopCapabilityError : public root::JoinError {
public:
    HopCapabilityError();
};
```

Thrown by `verify_hop` on capability mismatch. Catchable as
`root::JoinError`, `root::WorkError`, and `std::exception`.
`reason_code()` returns `root::JoinError::reason::hop_capability_mismatch`.

### `AggregateError`

```cpp
class AggregateError : public root::WorkError {
public:
    [[nodiscard]] std::span<std::exception_ptr const> causes_view() const noexcept;
    [[nodiscard]] std::vector<std::exception_ptr> causes_owned() const;
};
```

Thrown (wrapped in an `exception_ptr`) by `when_all` when both arms fail.

**Prefer `causes_owned()`** unless the caller is certain the `AggregateError`
outlives the use of the span. `causes_view()` returns a span whose lifetime is
bound to the `AggregateError` instance — moves or copies of the error
invalidate it.

---

## `Scope`

```cpp
class Scope {
public:
    Scope() noexcept;
    ~Scope();
    Scope(Scope&&) = delete;
    Scope(Scope const&) = delete;

    void track(root::TaskControl);
    void track(root::PostedControl);
    void track(root::OperationControl);

    void cancel(root::CancelReason) noexcept;
    [[nodiscard]] bool is_cancelled() const noexcept;
    [[nodiscard]] root::CancelReason cancel_reason() const noexcept;

    template<work_value T>
    Chain<T> admit(root::TaskJoinHandle<T>&&);

    template<work_value T, progress_capability Owner>
    Chain<T> admit(Owner&, root::PostedJoinHandle<T>&&);

    template<work_value T, progress_capability Driver>
    Chain<T> admit(Driver&, root::OperationJoinHandle<T>&&);

    template<work_value T, progress_capability Owner>
    Chain<T> admit_unbound(Owner&, root::PostedJoinHandle<T>&&);

    template<work_value T, progress_capability Driver>
    Chain<T> admit_unbound(Driver&, root::OperationJoinHandle<T>&&);
};
```

`Scope` provides parent-triggered best-effort cancellation for a group of
concurrent tasks. It is not movable or copyable.

### `track`

Adds a control handle for cancel propagation. If the scope is already cancelled,
calls `request_cancel()` on the passed handle immediately without storing it.
Throws `std::bad_alloc` if tracking storage cannot be extended.

**Fanout limit:** the supported design envelope is **n ≤ 32 tracked items per
Scope instance**. Debug builds assert at this boundary. Release builds do not
enforce the limit but may pay unbounded setup-time contention for higher
fan-outs (the tracking mutex serializes every `track` call).

The `cancel()` implementation swaps the registry vector under the mutex and
iterates outside it, so per-cancel mutex hold time is O(1). The contention
point is `track`/`untrack` serialization during setup, not during cancel.

Consumers needing higher fan-out must partition across multiple Scope instances
or file a follow-up to lift the limit via a concurrent registry.

### `cancel`

Fires `request_cancel()` on all tracked controls. First-writer-wins: subsequent
calls to `cancel()` are no-ops. Thread-safe.

**`cancel_reason()` reflects whichever cancel source fired first.** There is no
static precedence between `CancelReason::requested` and
`CancelReason::deadline`. When a parent `Scope::cancel(requested)` races a
`DeadlineScope` expiry, whichever thread arrives first defines the recorded
reason. Consumers that need to distinguish a deadline expiry from a manual
cancel should inspect the `DeadlineScope` directly rather than relying on the
recorded `cancel_reason()`.

### `admit`

Calls `track(jh.control())` then joins the handle synchronously
(`root::join(...)` blocks). Returns the completed `Chain<T>`.

`admit` overloads for `Posted` and `Operation` automatically bind the output
chain to the provided capability (`kind() = posted/operation`,
`bound_capability() = capability_id(owner/driver)`).

`admit_unbound` variants join without setting the capability — use when the
caller intends to bind the chain later via a hop function.

**`admit` blocks the calling thread until the join handle reaches a terminal
state.** Do NOT call `admit` from inside an `EagerChain` coroutine body.

---

## `DeadlineScope`

```cpp
class DeadlineScope : public Scope {
public:
    explicit DeadlineScope(std::chrono::steady_clock::time_point deadline);

    template<class Rep, class Period>
    explicit DeadlineScope(std::chrono::duration<Rep, Period> timeout);
};
```

A `Scope` that auto-cancels with `CancelReason::deadline` when the given wall
time arrives. Uses a `std::jthread` internally; the destructor joins the thread.

**When NOT to use:** `DeadlineScope` launches a jthread whose sole job is to
wait and then call `Scope::cancel`. For high-concurrency I/O where many
concurrent deadlines are managed per lane, this is too expensive. Use
`TimerService` + `LaneTimerScope` instead (see § Timer Service).

---

## Coroutine Support

Carrier coroutine types live in `conflux.work.carrier.coro`.

### `EagerChain<T>`

```cpp
template<work_value T>
class EagerChain;
```

A synchronous-only coroutine return type. A function returning `EagerChain<T>`
executes eagerly to completion on the calling thread — no suspension occurs.

```cpp
EagerChain<int> compute(Chain<int> a, Chain<int> b) {
    int x = co_await std::move(a);   // unwraps Chain<int>
    int y = co_await std::move(b);
    co_return x + y;
}
```

`EagerChain<T>` only allows `co_await` on `Chain<T>` and `EagerChain<T>` values.
Awaiting any other awaitable is a compile error (`await_transform` deleted).
`EagerChain<T>` itself is awaitable — `co_await std::move(eager)` inside
another `EagerChain` body extracts the inner result.

**Error handling:** if the awaited `Chain<T>` holds a failure, `co_await`
rethrows. The `EagerChainPromise` catches unhandled exceptions and stores them
as `Outcome<T>{Failure{...}}`. The coroutine never terminates abnormally.

**If the body somehow suspends** (which the type system should prevent, but may
arise from `await_transform` bypass), `EagerChain::chain()` returns a failure
outcome with `"EagerChain suspended: ..."` as the error message.

`EagerChain<T>` is move-only. Use `.chain()` to extract the `Chain<T>`, or
`co_await` it inside another `EagerChain`.

### Async `TaskJoinHandle` Awaiters (Phase 5c)

These enable `co_await` of `root::TaskJoinHandle<T>` inside any coroutine that
supports it (the coroutine frame must use an executor that drives the on-ready
callback).

```cpp
template<work_value T>
TaskHandleAwaiter<T> operator co_await(root::TaskJoinHandle<T>&&) noexcept;

template<work_value T>
TaskHandleChainAwaiter<T> await_chain(root::TaskJoinHandle<T>&&) noexcept;
```

`operator co_await` — resumes with `T` on success, rethrows on failure, throws
`CancelledError` on cancellation.

`await_chain` — resumes with `Chain<T>` carrying the outcome. Errors are
wrapped in the chain rather than thrown at the suspension point. Useful when
the caller wants to inspect the outcome before deciding whether to propagate.

**Both awaiters** install `try_set_on_ready` on the control block. If the
handle was already terminal at `await_suspend` time, they resume immediately
(no suspension). If the handle was already consumed by another awaiter,
`TaskHandleAwaiter` throws `JoinError`; `TaskHandleChainAwaiter` returns
a `Chain<T>` carrying the failure.

**`co_await PostedJoinHandle<T>` and `co_await OperationJoinHandle<T>` are
explicitly deleted.** Owner-affine and driver-affine resumption is not yet
implemented. From a non-coroutine context use `Scope::admit` to obtain a
`Chain<T>` (blocking); from a coroutine, await an intermediate `TaskJoinHandle`
that drives the operation.

---

## Timer Service

Carrier timer types live in `conflux.work.carrier.timer`.

### `TimerService`

```cpp
class TimerService {
public:
    explicit TimerService(io_uring* ring, std::uint64_t cqe_tag);
    ~TimerService() noexcept;

    TimerService(TimerService&&) = delete;
    TimerService(TimerService const&) = delete;

    void on_cqe(io_uring_cqe const* cqe) noexcept;
    [[nodiscard]] bool on_owner_thread() const noexcept;
    [[nodiscard]] int timer_fd() const noexcept;
};
```

`TimerService` manages per-lane deadline firing via a timerfd + io_uring read
loop. Construct it on the lane thread; all methods must be called on the same
thread.

**`ring`** — the lane's `io_uring` ring. Must outlive the `TimerService`.

**`cqe_tag`** — the `user_data` value that the lane event loop uses to identify
CQEs from this service. The event loop must call `on_cqe(cqe)` when it dequeues
a CQE with this tag.

**`on_cqe`** — call from the lane event loop immediately after
`io_uring_wait_cqe_timeout` returns the tagged CQE and **before**
`io_uring_cqe_seen`. Fires all expired callbacks, rearms the timerfd if further
deadlines remain, and resubmits the timerfd read SQE.

**Non-movable** — the timerfd read SQE holds a pointer to the service's
internal read buffer. Moving would invalidate that pointer while the SQE is
in-flight.

### `LaneTimerScope<Clock>`

```cpp
template<class Clock = std::chrono::steady_clock>
class LaneTimerScope {
public:
    LaneTimerScope(TimerService&, typename Clock::time_point deadline,
                   std::function<void()> callback);
    ~LaneTimerScope() noexcept;
    LaneTimerScope(LaneTimerScope&&) noexcept;
    LaneTimerScope& operator=(LaneTimerScope&&) noexcept;
    LaneTimerScope(LaneTimerScope const&) = delete;
    LaneTimerScope& operator=(LaneTimerScope const&) = delete;
};
```

RAII scope that registers a deadline callback with a `TimerService`. When
`deadline` arrives, the callback fires on the lane thread (called from
`on_cqe`). When the scope is destroyed before the deadline, the callback is
cancelled (lazy deletion via generation counter).

**`Clock`** — defaults to `std::chrono::steady_clock`. For other clocks, the
deadline is converted to a steady_clock time via `steady_clock::now() + (deadline - Clock::now())` at construction time.

**Move semantics** — moving the scope transfers cancellation ownership. The
source scope becomes empty (destructor is a no-op). Destroying the destination
scope cancels the timer.

**Callback contract:**
- Fires at most once, on the lane thread.
- Called from within `TimerService::on_cqe` — must not throw (exceptions are
  caught and emitted as diagnostics).
- Must not call back into `TimerService` in a way that would re-enter `on_cqe`.

**Internal compaction** — the service uses lazy deletion with a min-heap.
Cancelled entries become tombstones. Compaction runs when tombstones exceed
half the heap size or after 1024 cancellations, whichever comes first.

---

## Stream Types

Stream types live in `conflux.work.carrier.streams`.

### `DroppableSlot<T>`

```cpp
template<root::work_value T>
class DroppableSlot {
public:
    explicit DroppableSlot(root::TaskJoinHandle<T>&&);
    DroppableSlot(DroppableSlot&&) noexcept = default;
    DroppableSlot& operator=(DroppableSlot&&) noexcept = default;
    DroppableSlot(DroppableSlot const&) = delete;
    DroppableSlot& operator=(DroppableSlot const&) = delete;
    ~DroppableSlot() noexcept;

    template<class F> void on_drop(F&&) noexcept; // F: noexcept(Outcome<T>) -> void
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::optional<root::Outcome<T>> try_get() &&;
    [[nodiscard]] Chain<T> wait() &&;
    [[nodiscard]] DroppableSlotAwaiter<T> operator co_await() && noexcept;
};
```

`DroppableSlot<T>` wraps a `TaskJoinHandle<T>` with a drain-on-drop contract.
If the slot is destroyed without being consumed (`try_get`, `wait`, or
`co_await`), the destructor installs an `on_ready` callback that joins the
handle and calls the registered `on_drop` function when the task completes.
If the task is already terminal at destruction time, the drain runs synchronously.

**`on_drop(F&&)`** — registers a noexcept callback to receive the outcome when
the slot is drained by the destructor. Must be noexcept-invocable with
`root::Outcome<T>`. Ignored if the slot was already consumed.

**`try_get() &&`** — non-blocking. Returns `nullopt` if not yet terminal.
Consuming via `try_get`, `wait`, or `co_await` marks the slot consumed;
the destructor then skips the drain.

**`wait() &&`** — blocking join. Returns a `Chain<T>` with `kind() == task`.

**`co_await` (via `DroppableSlotAwaiter<T>`)** — async path. Suspends the
coroutine until the task is terminal, then resumes with `Chain<T>`. If the
awaiter is destroyed before resumption (e.g., coroutine cancelled), the
awaiter destructor performs the drain with the registered `on_drop` callback.

**Single-consumer invariant:** only one consumer may co_await or call `wait`/
`try_get` on a given slot. A second `try_set_on_ready` attempt terminates.

### `CoalescingSlot<T>`

```cpp
template<root::work_value T>  // requires T != void
class CoalescingSlot {
public:
    CoalescingSlot() noexcept = default;
    CoalescingSlot(CoalescingSlot&&) = delete;
    CoalescingSlot(CoalescingSlot const&) = delete;

    void commit(T value) noexcept;
    [[nodiscard]] std::optional<T> take() noexcept;
    [[nodiscard]] bool available() const noexcept;
};
```

Thread-safe single-value slot. `commit` overwrites any previously stored value.
`take` atomically removes and returns the value, or `nullopt` if empty.
Not movable — intended as a stable shared object.

---

## Deployment Requirements

### `RLIMIT_MEMLOCK` — Locked Memory for io_uring

`io_uring` operations that map shared memory between kernel and userspace (notably
`io_uring_setup_buf_ring` for zero-copy receive) require the process to be able
to lock those pages. If the locked-memory limit is too low the call fails with
`ENOMEM` (-12), surfaced as:

```
fatal: io_uring_setup_buf_ring failed: -12
```

The default `RLIMIT_MEMLOCK` on many systems is 8 MiB — sufficient for small
rings but not for production buffer-ring workloads.

**Fix:** set `LimitLOCKED=infinity` in the systemd unit, or `ulimit -l unlimited`
before launching the process.

```ini
# /etc/systemd/system/your-service.service
[Service]
LimitLOCKED=infinity
```

`TimerService` itself (timerfd + small SQE ring) stays well within the default
limit and is not affected.

---

## `JoinError::reason` Values

`root::JoinError::reason` is used in `root::JoinError` and its subclasses:

| Value | Meaning |
|---|---|
| `unspecified` | Default; existing throw sites not yet updated |
| `capability_mismatch` | Wrong capability passed to `join` |
| `thread_precondition` | Thread affinity precondition violated |
| `reentrant_pump` | Reentrant pump detected |
| `hop_capability_mismatch` | `verify_hop` / `HopCapabilityError` |
| `ready_callback_already_installed` | Second consumer tried to install `on_ready` hook |
