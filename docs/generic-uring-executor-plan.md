# Generic io_uring Executor Plan

Status: core executor landed; historical rationale with deferred domain
adapters.

Branch: `friction-fixes`.

## Goal

The generic execution-layer facility exists for keeping one `io_uring` owner
thread alive and running Conflux `root::Task<T>` coroutines on that owner. The
facility is work/execution infrastructure, not HTTP, socket, DNS, file, or
database API.

Keep this document as the rationale and acceptance checklist for the landed
core executor. Future work should be domain adapters or focused fixes, not a
new broad executor branch.

## Placement And Surface

Landed as a complete-only runtime component:

- CMake component: `work_uring_executor`
- CMake target: `conflux_work_uring_executor`
- Module: `conflux.work.uring_executor`
- Namespace: `conflux::work`
- Registry tier: `EXPLICIT|ADVANCED`
- Package surface: exported when built, not requestable through `find_package(... COMPONENTS ...)` in the first patch
- Feature gating: build when `CONFLUX_NEEDS_RUNTIME` is true; do not add a new user-facing feature flag in the first patch
- Aggregate surface: re-export only from `conflux.complete`, not from `conflux.work` or `conflux.extended`

Rationale:

- `conflux.work` is currently an extended aggregate for task/runtime primitives.
- Raw `io_uring`, socket/file async I/O, and low-level runtime escape hatches are complete-only.
- The executor context must expose `conflux::uring::RingRef`, `CompletionTable`, and `UserDataFn`; therefore the new module must stay out of the extended aggregate unless the API-surface policy is intentionally changed.

Documentation and metadata updated with the implementation:

- `cmake/components/RuntimeTargets.cmake`
- `cmake/ConfluxComponentRegistry.cmake` with an `EXPLICIT|ADVANCED` entry if the target is exported
- `docs/component-map.md`
- `docs/public-api-map.md`
- `docs/api-surface-profiles.md`
- `docs/conflux-work-root-api.md` or a new focused executor API doc linked from it
- `docs/api-surface-manifest.json` only for the complete aggregate if the module is re-exported by `conflux.complete`

## Current Building Blocks

- `WorkPool` owns ordinary worker threads for CPU/blocking offload.
- `RingLane` provides cross-thread enqueue/wake into an existing `io_uring` owner thread via `IORING_OP_MSG_RING`.
- Low-level async providers expect a live `io_uring`, a `CompletionTable`, and a stable user-data encoder.
- `SocketTaskRing` is socket-specific and non-owning; it remains outside the generic executor.
- `sync_wait_socket_task` is a caller-thread pump, not a persistent runtime.

## Public API

Landed in `conflux.work.uring_executor`:

```cpp
namespace conflux::work {

struct UringExecutorOptions {
    unsigned ring_entries = 256;
    std::size_t completion_slots = 64;
    std::size_t max_submission_queue = 4096;
    std::size_t lane_drain_budget = 0;
    bool require_single_issuer = true;
};

enum class UringExecutorState {
    starting,
    running,
    stopping,
    draining,
    stopped,
    failed,
};

class UringExecutorContext {
public:
    [[nodiscard]] conflux::uring::RingRef ring() const noexcept;
    [[nodiscard]] conflux::uring::CompletionTable& completions() const noexcept;
    [[nodiscard]] RingLane& lane() const noexcept;
    [[nodiscard]] std::uint64_t encode(std::uint32_t slot, std::uint32_t gen) const;
    [[nodiscard]] conflux::uring::UserDataFn user_data_encoder() const;
    [[nodiscard]] bool on_owner_thread() const noexcept;
};

class UringExecutor {
public:
    ~UringExecutor();

    UringExecutor(UringExecutor const&) = delete;
    UringExecutor& operator=(UringExecutor const&) = delete;
    UringExecutor(UringExecutor&&) = delete;
    UringExecutor& operator=(UringExecutor&&) = delete;

    void stop() noexcept;
    void join() noexcept;
    [[nodiscard]] UringExecutorState state() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] bool on_owner_thread() const noexcept;
    [[nodiscard]] int ring_fd() const noexcept;

    template<class Callable>
    [[nodiscard]] auto async_submit(Callable&& callable) -> root::Task</* inferred value */>;
};

[[nodiscard]] std::expected<std::unique_ptr<UringExecutor>, std::error_code>
try_make_uring_executor(UringExecutorOptions opts = {});

} // namespace conflux::work
```

`async_submit(callable)` accepts exactly one callable shape:

```cpp
root::Task<Value> callable(UringExecutorContext&);
```

The implementation constrains that callable equivalently to:

```cpp
template<class Callable>
requires requires(Callable& callable, UringExecutorContext& ctx) {
    { callable(ctx) } -> root_task_result;
};
```

The value type is inferred from `Task<Value>::value_type`. Callers must not need explicit template arguments, overload casts, or tag arguments.

The name is `async_submit`, not `submit`, because it returns a coroutine task and follows the project prefix policy.

## Construction Contract

Construction is non-throwing at the public boundary:

- `try_make_uring_executor(opts)` starts the owner `std::jthread`.
- The owner thread initializes the ring and `RingLane` before readiness is published.
- The factory blocks until the owner reports either `running` or `failed`.
- On startup failure, the factory joins the owner thread and returns `std::unexpected(std::error_code)` with the failing subsystem encoded where practical.
- A throwing constructor is out of scope for the first implementation.

Ring setup:

- The owner thread calls `io_uring_queue_init_params` from the owner thread, not the caller thread.
- Request `IORING_SETUP_SINGLE_ISSUER` when available.
- If `require_single_issuer == true` and the kernel rejects that setup, startup fails.
- If `require_single_issuer == false`, the implementation may retry without the flag, but must record that fallback in the startup diagnostic path.
- Every syscall/liburing return is checked. Failures include function name, entries, flags, and errno/error code in diagnostics where existing logging facilities allow it.
- The design is one thread per ring. No shared ring owner and no hidden worker pool.

Readiness barrier:

- No public `async_submit` can enqueue until the owner has constructed `RingLane`, called `lane.adopt_current_thread()`, and set state to `running`.
- `RingLaneOptions::allow_inline_on_owner` is `true` only after owner adoption; cross-thread callers never invoke the job inline.
- Pre-readiness submissions are impossible because the factory does not return before readiness.

## State Machine

| State | Meaning | `async_submit` | `stop` | `join` | destructor | `ring_fd` |
|---|---|---|---|---|---|---|
| `starting` | owner thread is initializing; factory has not returned | not externally reachable | request stop if factory cleanup needs it | wait for startup result | cleanup only inside factory | `-1` |
| `running` | accepting new work and CQEs | admit if queue capacity allows; otherwise return failed task | transition to `stopping`, reject new work, wake owner | transition to `stopping`, wake, wait for cooperative settlement | call `stop`, then `join`; may block like `std::jthread` destruction | live fd |
| `stopping` | no new work; unstarted jobs cancelled; admitted children requested to cancel | return failed task | idempotent | wait for cooperative settlement | call `join`; may block | live fd while owner exists |
| `draining` | owner is draining queued owner work and CQEs needed for safe shutdown | return failed task | idempotent | wait for cooperative settlement | call `join`; may block | live fd while owner exists |
| `stopped` | owner thread exited and ring destroyed | return failed task | no-op | no-op | no-op | `-1` |
| `failed` | startup failed before public construction | not externally reachable except error result | no-op | no-op | no-op | `-1` |

Admission and backpressure:

- `max_submission_queue == 0` is invalid and makes factory return `std::errc::invalid_argument`.
- The executor tracks admitted-but-not-started submissions separately from `RingLane`'s internal deque so it can enforce `max_submission_queue` before enqueueing.
- Queue-full submission returns `root::make_exception_task<T>(std::make_exception_ptr(std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "conflux.work.uring_executor: submission queue full")))`. It does not throw from `async_submit`.
- Submit-after-stop returns a ready cancelled task using `root::work_errc::cancelled_shutdown`.

## Owner Loop

The owner loop must be explicit and single-threaded:

```cpp
owner_thread:
    init io_uring with io_uring_queue_init_params
    construct CompletionTable
    construct RingLane with ring_fd, reserved wake user_data, lane_drain_budget
    lane.adopt_current_thread()
    publish running or failed through startup barrier

    while state is running/stopping/draining:
        rc = io_uring_submit_and_wait_timeout(ring, &first_cqe, 1, idle_timeout)
        if rc == -EINTR or rc == -EAGAIN: continue
        if rc == -ETIME:
            if draining and no live records, no runnable lane jobs, and no pending completions: break
            continue
        if rc < 0: record runtime failure, transition stopping, request child cancellation

        drain all currently visible CQEs:
            if low32(cqe.user_data) == UINT32_MAX:
                dispatch executor tag, including wake -> lane.drain()
            else:
                completions.dispatch(low32, high32, cqe.res, CqeFlags{cqe.flags})

        if stopping:
            drain lane until budget or empty
            cancel queued-not-started records
            transition stopping -> draining after admission is closed and queued-start cancellation is complete

        if draining and no live records, no runnable lane jobs, and no pending completions:
            break
```

Stop wake ordering:

- `stop()` changes state to `stopping` before it wakes the owner.
- `RingLane` must not be stopped until after no caller can need it to wake or drain shutdown work.
- If `RingLane::enqueue` fails while the executor is still `running`, `async_submit` rolls back admission and returns a failed task with a `std::system_error(std::make_error_code(std::errc::io_error), "conflux.work.uring_executor: ring wake failed")`.
- If wake fails during `stop()`, `stop()` records the failure and `join()` still waits; the owner loop must also use a bounded idle timeout so shutdown does not depend solely on the wake CQE.
- CQ overflow or malformed executor tags are runtime failures: record diagnostics, stop admission, request child cancellation, and drain toward shutdown.

## CQE Routing

Use an internal user-data namespace that cannot collide with `CompletionTable` slots:

- Completion-table CQEs keep the existing low-32 slot / high-32 generation encoding.
- Executor-owned CQEs use reserved slot `UINT32_MAX`; the high 32 bits carry a small executor tag.
- Executor wake CQE uses reserved slot `UINT32_MAX` plus the wake tag.
- `UringExecutorContext::encode(slot, gen)` refuses reserved slot `UINT32_MAX`; `CompletionTable` slot growth must never allocate that slot.
- `CompletionTable::reserve*` must gain an implementation guard so it fails before returning slot `UINT32_MAX`; the executor patch must add this guard or a checked reserve path before relying on reserved-slot routing.
- The executor decode helper is exactly: if `low32(user_data) == UINT32_MAX`, route by executor tag from `high32(user_data)`; otherwise call `completions.dispatch(low32, high32, res, flags)`.
- Caller-provided `wake_user_data` is removed from public options.

The first implementation supports only:

- executor wake CQEs;
- CQEs routed through the owned `CompletionTable`.

Additional executor-owned CQE services require an explicit tag allocator and are out of scope.

## Task Bridge Algorithm

`async_submit(fn)` must be implemented with the existing task model only:

1. Create an outer `Task<T>` and `TaskSource<T>` with cancellation enabled.
2. Store a submission record owning the decayed callable, outer source, outer control handle, admission token, and atomic record state.
3. Install an outer cancel hook before enqueue. If it wins `queued -> terminal`, it commits cancellation and releases admission. If the record is already `starting` or `child_bound`, it records the request and lets the owner/child path handle it.
4. Enqueue one owner job through `RingLane`.
5. On the owner thread, invoke the user callable only if it wins `queued -> starting`.
6. Invoke `fn(context)` and require the returned child to be `root::Task<T>`.
7. Convert the child to `TaskJoinHandle<T>` with `into_join_handle`.
8. Bind outer cancellation to the child control using `bind_child_for_cancellation` once the child exists, then transition `starting -> child_bound`. If cancellation was recorded while `starting`, immediately request child cancellation after binding.
9. Install `set_on_ready_or_run` on the child. The callback uses `join_ready`, not `blocking_join`, then commits success, failure, or cancellation into the outer source.
10. Clear child cancellation binding before committing the outer source.
11. Release the admission token when the child reaches a terminal state, or when pre-admission cancellation/rejection completes the outer task.

Submission record state:

| State | Owner | Cancellation behavior | Exit path |
|---|---|---|---|
| `queued` | caller/admission queue owns callable and source | cancel hook may commit/release only if it wins CAS `queued -> terminal` | owner job may invoke `fn` only if it wins CAS `queued -> starting`; otherwise queued lane entry is a tombstone |
| `starting` | owner thread owns invocation | cancellation records requested reason but does not commit; owner forwards it after child binding or commits it if no child is produced | callable returns child, throws, or completes outer as cancelled |
| `child_bound` | child join handle and outer source are owned by shared bridge state | outer cancel hook requests child cancellation through the bound child control | child ready callback wins `child_bound -> terminal`, clears binding, commits result, releases admission |
| `terminal` | no callable/child ownership remains | cancel hook is inert | admission token released exactly once |

Failure paths:

- Enqueue failure before owner admission rolls back the admission token and returns `root::make_exception_task<T>` with `std::system_error(std::make_error_code(std::errc::io_error), "conflux.work.uring_executor: ring wake failed")`.
- Callable throwing before returning a child commits that exception into the outer source and releases admission.
- Pre-admission cancellation commits `root::CancelReason::requested` or the captured requested `CancelReason` without invoking the callable.
- Shutdown cancellation commits `root::work_errc::cancelled_shutdown`.
- Every child terminal path calls `clear_child_for_cancellation(generation)` before committing the outer source.

Forbidden in this executor:

- `blocking_join` on the ring owner thread;
- arbitrary caller-thread task progress;
- invoking the user callable after pre-admission cancellation;
- storing references to caller-owned callables or request objects unless the caller explicitly captures them that way.

## Shutdown Contract

`stop()` is cooperative. The generic executor must not force-complete provider-owned kernel operations unless the provider has made that lifetime safe:

1. Atomically stop admission.
2. Mark queued-but-not-started submissions cancelled and commit their outer sources.
3. Request cancellation on admitted child tasks through their controls.
4. Wake the owner through `RingLane`.
5. Owner drains lane work until no runnable owner jobs remain.
6. Owner continues polling CQEs while admitted children or `CompletionTable` entries remain.
7. Providers are responsible for cancellation hooks that make their kernel operations settle and release their completion slots.
8. `CompletionTable::cancel_all()` is not part of the default executor shutdown path. It may be used only by a later explicit hard-stop API or by provider-specific code that proves late CQEs are stale-safe, including zero-copy notification handling.
9. Late/stale CQEs after slot generation changes are ignored by `CompletionTable`.
10. Once no admitted children, queued jobs, or pending completion entries remain, state becomes `stopped` and the ring is destroyed on the owner thread.

The first implementation does not provide a bounded hard-stop. `join()` and destruction may block until admitted children and providers settle cooperatively, matching `std::jthread`-style ownership rather than pretending arbitrary coroutine work can be killed safely. Tests must use cooperative children and bounded test-side timeouts; they must not assert that an intentionally uncooperative child can be destroyed without blocking.

## Domain Adaptation

The generic executor does not mention HTTP, DNS, sockets, files, or PostgreSQL.

HTTP async client follow-up:

- Build a socket-specific adapter in the HTTP async client component.
- The adapter owns stable shared state containing `SocketTaskRing`, cancellation submission plumbing, and a shutdown barrier.
- `SocketTaskRingOptions::submit_on_ring_owner` forwards through `UringExecutorContext::lane()` but must never capture stack-local `SocketTaskRing` by reference into a callback that can outlive the coroutine frame.
- The adapter must prove request/client/socket lifetimes cover all awaited socket operations.

Database follow-up:

- True ring-backed DB async work may create a DB-owned adapter over `UringExecutorContext`.
- Blocking libpq, CPU-bound conversion, and migration/setup work stay on `WorkPool` or explicit caller-owned threads.

## Tests

Focused runtime/work tests cover:

- Factory returns `std::unexpected` for invalid options such as `max_submission_queue == 0`.
- Factory handles `io_uring_queue_init_params` failure on unsupported hosts with a clear error path.
- Startup readiness race: many immediate post-factory submissions run on the owner thread, never the caller thread.
- Multi-producer submissions from several caller threads all complete and run on the owner.
- Queue capacity rejection returns a failed task and does not invoke the callable.
- Submit after `stop()` returns a cancelled task and does not enqueue work.
- A synchronous child coroutine completes successfully through the non-blocking bridge.
- An asynchronous child coroutine completes after an `IORING_OP_NOP` CQE routed through `CompletionTable`.
- Pre-admission cancellation does not invoke the callable.
- Post-admission cancellation reaches the child task through `bind_child_for_cancellation`.
- Destructor with cooperative pending work stops and joins.
- Shutdown with a cancellable pending completion drains until the provider releases its slot without generic force-completion.
- Late/stale CQE routing is ignored after generation changes.
- Import smoke: explicit `import conflux.work.uring_executor;` works.
- API-surface smoke: `import conflux.extended;` does not expose the executor unless policy is intentionally changed; `import conflux.complete;` exposes it if added to the complete aggregate.
- Package/export smoke includes the new explicit component when exported; it is not requestable through `find_package(... COMPONENTS ...)` in the first patch.

## Acceptance Checks

- [x] The executor is in a work/execution component, not HTTP or socket I/O.
- [x] The executor is complete-only or explicit-only; raw `io_uring` types do not leak into the extended `conflux.work` aggregate.
- [x] `async_submit` accepts `[](UringExecutorContext&) -> root::Task<T>` without explicit template arguments, overload casts, or tag arguments.
- [x] Owner-thread startup uses `io_uring_queue_init_params`, checks every return value, requests `SINGLE_ISSUER`, and publishes readiness only after `RingLane` owner adoption.
- [x] Submitted work runs only on the ring owner thread.
- [x] Admission has a bounded queue contract and deterministic failed-task behavior for full/stopped executors.
- [x] Executor wake CQEs use reserved slot `UINT32_MAX`; `CompletionTable` CQEs retain the existing slot/generation encoding and cannot allocate the reserved slot, enforced by a concrete `CompletionTable` guard.
- [x] The task bridge uses `TaskJoinHandle`, `set_on_ready_or_run`, `join_ready`, and child cancellation binding; it never uses `blocking_join` in the owner loop.
- [x] Pre-admission cancellation does not invoke the caller callable.
- [x] Post-admission cancellation propagates to the child task through the existing `root::Task` cancellation model.
- [x] Shutdown rejects new work, cancels queued work, requests child cancellation, drains provider-owned completions cooperatively, and joins without generic force-completion.
- [x] Focused runtime tests cover startup, multi-producer admission, cancellation, CQE routing, shutdown, import surface, and package surface.
- [x] `docs/component-map.md`, `docs/public-api-map.md`, API-surface docs/manifest, and work API docs are updated.
- [x] The landed executor patch did not require large verbatim code moves.

## Non-Goals

- Do not move `SocketTaskRing` into `conflux.work`.
- Do not make HTTP client APIs the first public shape of the runtime.
- Do not hide blocking DB/libpq work on the ring executor.
- Do not add overloads that require callers to disambiguate with casts or explicit template arguments.
- Do not replace `WorkPool`; CPU/blocking work still belongs there.
- Do not add graceful-drain policy, metrics, or domain-specific adapters in the first generic executor patch.

## Expected Follow-Up Lane

The one-shot HTTP async client convenience layer landed. A persistent HTTP
client reactor remains a separate design in `docs/http-client-reactor-plan.md`.
A later DB lane should only use this executor for true ring-backed async work;
blocking libpq or CPU-bound work stays on `WorkPool`.
