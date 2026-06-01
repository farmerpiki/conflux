# `conflux.work.root` API Reference (Current Public Contract)

This document describes the current public interface and behavior contract of
`conflux.work.root` as implemented in this repository.

For broader pre-v1 compatibility and breakage expectations across HTTP + work
surfaces, see `docs/pre-v1-migration-contract.md`. For executor placement rules,
see `docs/execution-model.md`; for code-review guidance on placement and
execution-name prefixes, see `docs/concurrency-naming-model.md`.

## Import

```cpp
import conflux.work.root;
```

## Module Surface

`conflux.work` re-exports `conflux.work.root` and adds the executor layer:

- `WorkPool` — thread-pool executor with direct stealing queues, direct inject/no_stealing rings, and selectable stealing/no_stealing queue modes
- `RingLane` — io_uring-coupled single-threaded executor
- `async_run_on(pool, fn) -> Task<T>` — schedule work on a pool or lane
- `join_all(tasks...) -> Task<std::tuple<Ts...>>` — wait for all tasks and
  return a tuple of successful values

All task progress is executor-owned. There is no supported `Task<T>` model that
runs outside an executor or falls back to ad-hoc caller-thread execution. The
current executor backends are the work/uring combination: `WorkPool` and
io_uring-coupled ring executors.

All root async vocabulary (`Task<T>`, `Posted<T>`, `Operation<T>`, source/control
types, `Outcome<T>`, join, abandon APIs) lives in `conflux.work.root`. Import
`conflux.work` when you also need `WorkPool` or `RingLane`.

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
- `Cancelled` (`CancelReason::{requested,abandoned,shutdown,deadline}`)

And wrapped by:

- `Outcome<T>` / `Outcome<void>`
- `OutcomeKind::{success,failure,cancelled}`

Inspection APIs:

- `kind()`, `is_success()`, `is_failure()`, `is_cancelled()`
- arm accessors: `success()`, `failure()`, `cancelled()`
- total visitor: `visit(F&&)` for `&`, `const&`, `&&`
- per-arm callback: `match(OnSuccess&&, OnFailure&&, OnCancelled&&)` for `&&` and `const&`

Result extraction helpers:

- `value(Outcome<T>&&) -> T` (free function; throws `FailureError` or `CancelledError`)
- `value(Outcome<void>&&) -> void`
- member `value() &` / `value() const &` / `value() &&` — same semantics, on the object directly

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

template<work_value T, typename OnCancel>
std::pair<Task<T>, TaskSource<T>> make_cancellable_task_source(OnCancel);

template<class Callable>
Task<Value> make_cancellable_task(Callable);

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
The request can carry a `CancelReason`; old no-argument calls mean
`CancelReason::requested`.

`make_cancellable_task_source<T>(on_cancel)` is the external-producer helper for
the common cancellable bridge shape. It creates a cancellable `Task<T>` and
installs `on_cancel(CancelReason)` as the source cancel hook. The hook is still
advisory: the external producer must complete the source with `try_set_value`,
`try_set_exception`, `try_set_error`, or `try_set_cancelled` when the underlying
operation actually finishes.

`make_cancellable_task(fn)` is the task-authoring helper for bodies of shape
`fn(Cancellation) -> T` and `fn(Cancellation) -> Task<T>`. The `Cancellation`
view exposes `requested()`, `reason()`, `stop_token()`, `throw_if_requested()`,
and `await(Task<T>)`. `await(child)` binds parent cancellation to the child task
until the child reaches a terminal outcome. Returning from a sync body commits
success, throwing `CancelledError` commits cancellation, and throwing any other
exception commits failure. Returning a child task flattens the child outcome and
forwards parent cancellation to that child. Long-running CPU work should use an
executor helper such as `async_run_cancellable_on` once available, because direct
synchronous bodies run in the caller's admission path.

`async_run_cancellable_on(target, fn)` is the explicit work-pool/queue variant
for synchronous cancellable bodies. It passes `Cancellation` to `fn`, skips the
body if cancellation was already requested before the queued job starts, and maps
`CancelledError` to a cancelled outcome. Running jobs remain cooperatively
cancelled; cancellation never kills a worker thread.

## Application Guidance

Long-running jobs should expose progress through an application-owned channel or
callback today. The root API provides cancellation, ownership, and terminal
outcomes; it does not yet standardize a public progress/event channel. Keep the
channel tied to the same owner as the work so cancellation, shutdown, and UI
handoff do not outlive the backing state.

Use `join_all` when all sibling tasks must complete. Use `work::race` for
first-completer or first-success flows where losers should be cancelled through
their controls. There is no separate public `when_any` or
`when_all_fast_fail` facade in the current preview; adding one should preserve
the same cancellation-reason and loser-ownership contracts as `work::race`.

UI or main-thread applications should not block the UI thread with
`blocking_join`. Run work on an explicit `WorkPool`, request cancellation from
the UI owner when the view or operation closes, and post terminal results back
through the UI framework's own dispatcher. Keep captured UI state weak or
owner-checked so a late result after cancellation cannot mutate a destroyed
view.

For HTTP handlers, the same rule applies: ring-thread handlers should submit or
await explicit work, then return a response on the documented handler path. Do
not hide arbitrary long-running work behind synchronous handler bodies.

## Optional Allocation Diagnostics

The CMake option `CONFLUX_WORK_ALLOC_STATS` enables relaxed counters for
`conflux.work.root` allocation diagnostics. It defaults to `OFF` so the hot path
does not pay atomic counter cost in normal builds.

```cpp
struct TaskAllocationStats {
    uint64_t control_block_allocations;
    uint64_t control_block_deallocations;
    uint64_t coroutine_frame_allocations;
    uint64_t coroutine_frame_deallocations;
};

TaskAllocationStats task_allocation_stats() noexcept;
void reset_task_allocation_stats() noexcept;
```

With the option disabled, both functions are still available but the snapshot is
zeroed and reset is a no-op. The counters are intended for benchmarks and for
validating current `ControlBlockModel<T>` pooling behavior; they are not a stable
telemetry API. Coroutine frame counters report actual allocation calls through
the work-root promise allocator, so compiler coroutine-allocation elision may
legitimately keep them at zero.

`CONFLUX_WORK_CORO_FRAME_POOL` enables the worker coroutine-frame pool. For
`Task<T>` promise frames, `conflux.work.root` uses process-lifetime mmap-backed
size buckets for small/medium frames and falls back to the synchronized PMR pool
for oversize frames or mmap failure. The bucket pool is deliberately
process-lifetime and mutex-protected, rather than thread-local, because `Task<T>`
coroutines may suspend and can be destroyed from a different worker thread than
the one that allocated the frame. Sanitizer builds disable the mmap bucket path
and keep the safe PMR fallback.

The CMake option `CONFLUX_WORK_QUEUE_STATS` enables relaxed `WorkPool` queue,
steal, park, wake, and queue counters for contention profiling. It defaults to `OFF` so
normal builds keep the existing hot path. The API is always present:

```cpp
struct WorkPoolQueueStats {
    uint64_t enqueue_attempts;
    uint64_t enqueue_stopped_rejections;
    uint64_t enqueue_full_rejections;
    uint64_t admission_lock_acquisitions;
    uint64_t admission_lock_contentions;
    uint64_t local_lock_acquisitions;
    uint64_t local_lock_contentions;
    uint64_t steal_lock_acquisitions;
    uint64_t steal_lock_contentions;
    uint64_t local_pushes;
    uint64_t local_push_full;
    uint64_t inject_pushes;
    uint64_t inject_push_full;
    uint64_t local_pop_attempts;
    uint64_t local_pop_hits;
    uint64_t inject_pop_attempts;
    uint64_t inject_pop_hits;
    uint64_t steal_rounds;
    uint64_t steal_victim_checks;
    uint64_t steal_hits;
    uint64_t jobs_run;
    uint64_t wake_one_calls;
    uint64_t wake_one_futex_wakes;
    uint64_t wake_one_elided_no_parked;
    uint64_t wake_all_calls;
    uint64_t wake_all_futex_wakes;
    uint64_t park_attempts;
    uint64_t park_recheck_skips;
    uint64_t futex_waits;
    uint64_t queue_full_token_discards;
    uint64_t token_take_failures;
};

WorkPoolQueueStats WorkPool::queue_stats() const noexcept;
void WorkPool::reset_queue_stats() noexcept;
```

With the option disabled, snapshots are zeroed and reset is a no-op. With the
option enabled, counters use relaxed atomics and are intended for benchmark and
profiling runs only. They are not a stable telemetry surface and should not be
used for correctness decisions.

The `*_lock_contentions` fields count a failed first `try_lock()` probe before
falling back to the normal blocking mutex acquisition. They are deliberately
coarse: they identify whether admission, owner-local deque, or steal-victim
locks are contended enough to justify a deeper redesign, without timing critical
sections or changing scheduling semantics in default builds.

## Source Contract

`BasicSource<T, Category>` / `BasicSource<void, Category>` APIs:

- `try_set_value(Success<T>)` / `try_set_value(Success<void> = {})`
- `try_set_exception(std::exception_ptr)`
- `try_set_error(std::error_code)` / `try_set_error(std::error_code, std::string_view)`
- `try_set_cancelled(work_errc = work_errc::cancelled_requested)`
- `try_set_cancelled(CancelReason)`
- `install_cancel_hook(fn)`
- `stop_token()`

Rules:

- exactly one terminal `try_set_*` call wins
- source destruction without a terminal `try_set_*` performs fallback abandoned cancel
- source destruction does **not** fire the installed cancel hook; it fires the
  `on_ready` callback only — if cleanup must run on all teardown paths, call
  `request_cancel()` before releasing the source, or include the cleanup in the
  `on_ready` callback

### Set/Cancel-Hook Race Guarantee

`install_cancel_hook` followed by `try_set_*` on a different thread is safe.
The cancel hook is **advisory and independent of terminal state.** A call to
`request_cancel(reason)` fires the hook at most once but does NOT prevent a
subsequent `try_set_value` from winning the terminal state. Both can happen:
hook fires (cancel requested) and the terminal state is success (because
the work completed before the cancel took effect). This is by design —
`request_cancel` is a hint, not a preemption. Callers must not assume that a
fired cancel hook means the source cannot set success.

Hook installation after terminal completion returns `false` and does not run the
hook. Hook installation after a cancel request but before terminal completion runs
the hook synchronously on the calling thread with the stored first cancellation
reason, unless a hook was already installed.

**`on_ready` callback and abandon:** when the producer side abandons the control
block without calling a terminal `try_set_*` (e.g., `~TaskSource` without a set),
the abandon path transitions the control block to a terminal cancelled state and
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

- `request_cancel(CancelReason = CancelReason::requested) -> bool`
- `stop_token() -> std::stop_token`
- `cancel_requested() -> bool`
- `cancellation_reason() -> std::optional<CancelReason>`
- `ready() -> bool`
- `state() -> WorkState`
- `can_join_with(CapabilityId) -> bool`
- `static constexpr category() -> ControlCategory`
- `try_set_on_ready(move-only void() callback) -> result with status/rejected_fn`
- `clear_on_ready() -> ClearOnReadyStatus`
- `set_on_ready_or_run(F&&) noexcept` (convenience: installs or runs immediately)

`request_cancel(reason)` returns `true` only for the first successful request
before terminal completion. The first successful request stores `reason`;
subsequent requests return `false` and do not change the stored reason.

Cancel hook semantics:

- single installed hook at most
- first successful cancel request runs hook synchronously with the stored reason
- late hook installation after a cancel request runs immediately with the stored reason
- hook must not throw; throwing hook terminates

### Ready Callback APIs

`try_set_on_ready` installs a one-shot `void()` callback that fires when the
control block reaches a terminal state. Returns an implementation-defined result
with this shape:

```cpp
struct /* implementation-defined */ {
    ReadyRegistration status;
    /* move-only void() callback */ rejected_fn; // non-null if not installed
};

enum class ReadyRegistration : std::uint8_t {
    installed,           // callback queued; will fire on commit thread
    already_ready,       // already terminal; caller must run rejected_fn
    already_installed,   // second install attempt; rejected_fn returned
    empty,               // control is empty (moved-from); rejected_fn returned
};

enum class ClearOnReadyStatus : std::uint8_t {
    cleared,             // callback removed; will not fire
    in_flight,           // commit already in progress; callback may fire anyway
    already_terminal,    // terminal reached; callback already fired or will fire
    not_armed,           // nothing was installed
};
```

`set_on_ready_or_run(F&&)` — if `already_ready`, runs `fn` immediately on the
calling thread; otherwise installs it. `already_installed` and `empty` are
silently dropped.

`co_await` and `outcome()` use this same one-shot ready callback. If another
callback is already installed on the control block, awaiting the task fails
deterministically with `JoinError::ready_callback_already_installed`. After
resumption, they extract the result through the ready-only path, so they do not
fall back to `blocking_join(...)`.

## Join, Value, and Join Handles

Join and value APIs:

- `blocking_join(Task<T>&&)` — explicit blocking join; waits for terminal state
- `blocking_join(Owner&, Posted<T>&&)` — explicit blocking join; validates owner
- `blocking_join(Driver&, Operation<T>&&)` — explicit blocking join; validates driver
- `blocking_join(...)` — blocking wait for a task or join handle
- `try_join_ready(...)` — ready-only join over the same result/handle overload
  set; returns `nullopt` if the task is still pending and does not consume it
- `join_ready(...)` — ready-only join over the same result/handle overload set;
  throws `JoinError::not_ready` if the task is still pending
- same overload set for `TaskJoinHandle<T>`, `PostedJoinHandle<T>`,
  `OperationJoinHandle<T>`
- `value(...)` overloads use the explicit blocking join path

Join handles are produced by:

- `into_join_handle(Task<T>&&)`
- `into_join_handle(Posted<T>&&)`
- `into_join_handle(Operation<T>&&)`

Join handles expose `.control()` (returns the matching `*Control` type) and
`operator bool()` (false if moved-from or empty).

Capability query helpers:

- `can_join(Owner&, PostedControl const&) -> bool`
- `can_join(Driver&, OperationControl const&) -> bool`
- `joinable(Cap const&, PostedJoinHandle<T> const&) -> bool`
- `joinable(Cap const&, OperationJoinHandle<T> const&) -> bool`

Strict join and dropped-outcome helpers:

- `require_join(Task<T>&&) -> JoinTask<T>` wraps a task in the strict RAII variant
- `spawn(fn, loc = current) -> Task<T>` calls a `fn` that returns `Task<T>` and records the spawn location; dropped tasks auto-detach
- `spawn_strict(fn, loc = current) -> JoinTask<T>` records the spawn location and terminates on dropped live strict task
- `JoinTask<T>::detach_to_task() && -> Task<T>` downgrades strict ownership to auto-detach task ownership
- `set_dropped_outcome_sink(fn)` installs a process-wide sink called as `fn(std::source_location, OutcomeKind, std::exception_ptr)` for dropped failure/cancelled outcomes

Contract behavior:

- `blocking_join(...)`, `try_join_ready(...)`, and `join_ready(...)` on a
  moved-from/non-live object throw `JoinError`
- posted/operation joins validate capability identity and throw
  `JoinError` on mismatch before readiness checks
- `try_join_ready(...)` never blocks and never consumes pending work
- `join_ready(...)` never blocks; pending work throws `JoinError::not_ready`
- dropping live `Task<T>`, `Posted<T>`, or `Operation<T>` auto-detaches; success is discarded, failure/cancelled outcomes are reported to the dropped-outcome sink when installed
- live `TaskJoinHandle<T>`, `PostedJoinHandle<T>`, `OperationJoinHandle<T>`, and `JoinTask<T>` destructors terminate; consume, join, convert back to `Task<T>`, or abandon explicitly

## Capability Identity Contract

Capability identity types:

- `CapabilityId { void const* address; void const* type_tag; }`
- `capability_id` CPO (`tag_invoke` customization)
- opt-in trait: `template<class T> inline constexpr bool enable_address_capability_v = false`

`progress_capability` requires `capability_id(cap)` to be available and
`noexcept`, returning `CapabilityId`.

Specialize `enable_address_capability_v<CapabilityType> = true` to opt into
address identity with a per-type static tag, or provide a custom
`tag_invoke(capability_id_t, cap)` overload for another identity scheme. The
per-type tag prevents first-base subobject aliasing from collapsing distinct
capability types.

```cpp
struct OwnerCap {};

namespace conflux::work::root {
template<>
inline constexpr bool enable_address_capability_v<OwnerCap> = true;
}
```

## Abandonment APIs

Abandonment entry points:

- `abandon_to(<result-or-handle>, sink)` — blocking; sink runs on commit thread or caller thread if already terminal
- `try_abandon_to(TaskJoinHandle<T>&&, sink) -> AbandonStatus`
- `try_abandon_to(PostedJoinHandle<T>&&, sink) -> AbandonStatus`
- `try_abandon_to(OperationJoinHandle<T>&&, sink) -> AbandonStatus`
- `guard_abandon(value)` returns `scoped_abandon`

`try_abandon_to` is the non-blocking variant. Returns `AbandonStatus`:

```cpp
enum class AbandonStatus : std::uint8_t {
    installed,          // sink queued; will fire when terminal
    already_abandoned,  // already in abandoned state; sink not called
    empty,              // handle was empty or moved-from
};
```

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

## Executor Contract

`WorkPoolOptions`:

```cpp
enum class WorkPoolQueueMode {
    stealing,    // per-worker job deque + victim stealing
    no_stealing,   // atomics-only direct job rings, no stealing
};

struct WorkPoolOptions {
    size_t threads = 0;                // 0 => hardware_concurrency, at least 1
    size_t max_inject_queue = 4096;    // total external producer queue target
    size_t inject_queue_shards = 0;    // 0 => one inject ring per worker
    size_t local_queue_capacity = 1024;// per-worker local queue/ring bound
    WorkPoolQueueMode queue_mode = WorkPoolQueueMode::stealing;
    uint32_t spin_before_park = 256;
    int numa_node = -1;
    bool pin_workers = false;
    std::string worker_name_prefix = "conflux-work";
    std::function<void(std::exception_ptr)> raw_exception_sink = {};
};
```

`enqueue(job)` returns `false` if the pool is stopped or the relevant queue is
full. Jobs submitted from a worker in the same pool first use that worker's
bounded local queue; other producers use sharded bounded direct-job inject rings.
`max_inject_queue` is divided across inject shards; `inject_queue_shards == 0`
picks one inject ring per worker.

Default `stealing` mode keeps the mutex-protected local job deque and victim
stealing behavior. It skips victim scans while no worker-local jobs are queued,
so external-producer workloads do not pay the steal-lock path when stealing
cannot produce work. `no_stealing` mode uses bounded atomics-based direct job
rings for local worker submissions and external injection, removes victim
stealing, and uses an atomic admission gate instead of the admission mutex for
stop/drain coordination.

Use `no_stealing` for bounded external offload pools where tasks are independent
and load is already spread through inject rings. Keep default `stealing` for
recursive fanout or uneven worker-local production, where victim stealing is the
mechanism that redistributes local backlog.

`stop()` is a hard stop: it rejects new work, requests worker shutdown, and may
abandon queued jobs that have not started. This is also the destructor behavior.
Call `wait()` after `stop()` when you need to join workers before destroying
dependent state.

`drain_and_stop()` rejects new work, wakes workers, waits until already queued
and running jobs finish, then stops and joins the pool. Use it when queued raw
jobs must run before shutdown. It can block indefinitely if a job blocks
indefinitely.

Raw jobs submitted through `enqueue()` must not throw unless
`raw_exception_sink` is configured. If a raw job throws and a sink is present,
the sink receives the `std::exception_ptr`; exceptions thrown by the sink are
suppressed. `async_run_on(pool, fn)` does not use this sink for normal callable
failures because it reports them through the returned task.

Blocking waits are not assisted by `WorkPool`. A job running on a pool worker
must not synchronously wait for other work that is queued only to the same pool,
especially with `threads == 1`; doing so can deadlock. Use continuations,
`try_join_ready(...)`/`join_ready(...)` after readiness is known, separate
capacity, or wait from a non-worker thread.

`join_all(tasks...)` has wait-all semantics. It completes only after every input
task is terminal. The returned task preserves the first observed failure and
does not aggregate additional failures. If an input task is cancelled,
`join_all` requests cancellation on siblings and still waits for all of them to
finish before committing cancelled. Cancelling the returned task is inert; it
does not cancel children.

## Exceptions

- `WorkError` base class
- `JoinError` for join context/liveness/capability issues
- `FailureError` and `CancelledError` for `value(...)` extraction

## Implementation Notes

### Callback Inline Buffer

Control callbacks use an internal small move-only callable wrapper with a
**32-byte inline buffer** aligned to `std::max_align_t`. Callables that fit
within 32 bytes are stored inline with no heap allocation. Larger callables
heap-allocate.

Cancel hooks and `on_ready` callbacks that capture multiple values (e.g.,
`(fd, ring_handle, context_ptr)`) may exceed the 32-byte limit and heap-allocate.
Callers on hot paths who want to avoid this allocation should measure with the
`callable_erasure_*` benchmarks and restructure captures to fit the buffer.

No public size-hint knob is exposed. If profiling reveals a consistent overflow
pattern, a public size configuration can be added as a follow-up.

## Non-Goals of This Layer

This layer is the root contract surface. It does not define higher-level
combinator/carrier ergonomics, structured concurrency, or sender/receiver
interop adapters.
