# Proposal: Composable Race and Reasonful Cancellation Semantics

Status: open TODO.
Branch: `work/composable-race`

**Final tightening pass 3:** incorporates request-cancel loser ownership, zero-allocation borrowed labels by default, a shaped `race_owned_labels` wrapper, in-flight ready-callback lifetime, and `leave_running` ownership semantics.

## Status

Active proposal, final design pass. The generalized direction is accepted. Treat the patch as ready only after the contracts in this document land in order: reason propagation, exclusive registration, callback lifetime/quiescence, progress-domain binding, capability-safe extraction, explicit label lifetime policy, and explicit loser ownership.

This version replaces the earlier timeout-only framing, earlier pump-first framing, and the previous composable-race draft. It incorporates a verification pass against the current `conflux.zip` snapshot from 2026-05-26.

## Verification summary against current source

Current code already has most of the substrate, but not the exact primitive:

- `root::Task<T>`, `TaskJoinHandle<T>`, `TaskSource<T>`, `TaskControl`, ready callbacks, cancellation hooks, and join helpers exist.
- `TaskControl::request_cancel()` exists, but has no cancellation-reason parameter.
- `TaskSource<T>::install_cancel_hook(...)` accepts `CancelReason`, but the control layer currently invokes request-triggered hooks with `CancelReason::requested` only.
- `TaskSource<T>::try_set_cancelled(...)` publicly accepts `work_errc`; the underlying control block accepts `CancelReason`, but public callers mostly commit default/requested cancellation.
- `carrier::race(Chain<T>, Chain<T>)` exists, but it is outcome-level and two-way, not live async orchestration.
- `carrier::TimerService`, `DeadlineScope`, and `LaneTimerScope` exist, but are not yet exposed as typed race participants.
- Socket async operations already install owner-thread-safe cancellation hooks that marshal cancel SQEs through `SocketTaskRing::submit_on_owner(...)`.
- Socket timeout overloads already hand-code a specialized operation-vs-timeout race.
- `ControlBlockBase` already exposes `can_join_with(...)` and `required_capability()`, so live race outcome extraction must respect the same owner/capability rules as existing join helpers.

Verified source constraints that materially change the proposal:

- `BasicControl::set_on_ready_or_run(...)` silently returns on `ReadyRegistration::already_installed`; a race must **not** build on it blindly. Race registration must use `try_set_on_ready(...)` directly and treat `already_installed` as an invalid/shared participant error. Current source: `src/work/root_core.cxx:1813-1828`.
- `try_set_on_ready(...)` supports only one callback slot. It returns `installed`, `already_ready`, `already_installed`, or `empty`. Current source: `src/work/root_core.cxx:1121-1154`.
- Coroutine awaiters already treat `already_installed` as an error path rather than ignoring it. Current source: `src/work/root_tasks.cxx:39-55`.
- Blocking socket wait currently pumps `io_uring` CQEs itself. A generic race that “does not drive io_uring” cannot provide `request_cancel_and_wait` semantics in blocking contexts unless progress is otherwise guaranteed. Current source: `src/socket_io/socket_io_blocking.cxx:27-103`.
- The control layer already gates joins through `can_join_with(...)` / `required_capability()`. Current source: `src/work/root_core.cxx:842-847`, `src/work/root_tasks.cxx:741-766`.
- Several cancel hooks ignore their received `CancelReason` and commit default/requested cancellation, for example socket send/recv/sleep paths. Current source examples: `src/socket_io/socket_io_coro_impl.cxx:126-143`, `src/socket_io/socket_io_coro_impl.cxx:185-202`, `src/socket_io/socket_io_coro_impl.cxx:1373-1389`.
- `join_all` currently uses `set_on_ready_or_run(...)` and request-cancels siblings with no reason. That is acceptable as current behavior, but race should not copy that registration/cancellation shape. Current source: `src/work_api.cxx:583-663`.
- `clear_on_ready()` can return `in_flight`; registration rollback cannot assume a cleared callback is gone. Any race state captured by an installed callback must remain alive until that callback has quiesced. Current source: `src/work/root_core.cxx:1155-1179`, `src/work/root_core.cxx:1530-1554`.
- The current proposal shape consumes live handles. Therefore `leave_running` cannot mean “caller still owns the loser” for moved participants. If exposed for consumed live participants, race must retain loser handles in detached cleanup state until terminal outcome is discarded/recorded.
- `request_cancel` has the same ownership problem as `leave_running` for consumed live losers. It returns before loser terminal completion, but `TaskJoinHandle` destructors terminate when still live. Race must transfer request-cancel losers into a detached loser sink/abandon path, or reject this policy for consumed live participants unless an external owner adapter observes terminal completion. Current source: `src/work/root_tasks.cxx:416-419`.
- Returning `std::string_view` labels from `race_result` or aggregate exceptions is only safe when labels borrow storage that outlives the result/exception. The core race must not copy labels behind the user's back, because labels are diagnostics and should be zero-allocation by default. Dynamic labels require an explicit owned-label wrapper.

## Decision

Adopt the generalized race direction, tightened around eight implementation-blocking contracts:

1. **Exclusive observation/registration:** live race consumes participants and owns their single ready callback slot.
2. **Rollback-safe callback lifetime:** installed callbacks capture race state safely; setup failure, cancellation, and disarm paths keep state alive until any in-flight callback has quiesced.
3. **Progress-domain guarantee:** `request_cancel_and_wait` is valid only when an owner/executor/pump is guaranteed to make losers complete.
4. **Reasonful cancellation end-to-end:** reason must flow through request, hooks, operation cancellation, and final cancelled outcome where the operation can observe it.
5. **Capability-safe outcome extraction:** a race may observe readiness through controls, but may only take outcomes in a capability-safe context.
6. **Explicit label lifetime:** labels are optional diagnostics. Bare `race` is zero-allocation and returns borrowed `std::string_view` labels under a documented lifetime precondition. An owned-label wrapper is provided for dynamic labels.
7. **Explicit loser ownership:** for consumed live participants, every loser has exactly one terminal owner under every loser policy, including `leave_running` and `request_cancel`.
8. **Non-duplicated winner policy:** Stage 1 has `first_completion` and `first_success`; the previous `first_value_or_all_failed` wording was effectively duplicate and is removed for now.

The proposal remains worth implementing. It should be implemented in narrow stages, starting with root-layer reason propagation and registration/progress/lifetime tests before any subsystem adoption.

## Problem statement

Conflux currently has several separate ways to wait, time out, and cancel work:

- `root::blocking_join(...)` waits on task state and does not drive `io_uring` progress.
- Socket blocking wait drives socket-ring CQEs with socket-specific user-data decoding and timeout behavior.
- File and iopoll blocking waits have similar pump-until-ready shapes, but use their own dispatch and polling rules.
- Socket async cancellation must be owner-thread-safe and usually posts cancellation back through the ring owner.
- Async client cancellation forwards into the active child operation.
- Carrier chains can race already-materialized outcomes, but cannot orchestrate live I/O completion.

A generic pump cannot safely call `request_cancel()` and mutate lower-level state. Cancellation cleanup belongs to each operation owner. However, making every operation grow its own timeout/shutdown/disconnect parameter scales badly.

The right abstraction boundary is:

```text
race layer:  choose first completer, request loser cancellation through controls, report outcome
owner layer: define what cancellation means, where it runs, how cleanup completes, how progress happens
```

## TODO Goals

- [ ] One composable race primitive usable by internal code and users.
- [ ] Support N participants, not just two.
- [ ] Support value candidates and non-value triggers.
- [ ] Support task-vs-task and task-vs-ready-chain in Stage 1.
- [ ] Support owner-bound operation/post races only with explicit owner/capability binding.
- [ ] Support nested races inside larger tasks/chains.
- [ ] Preserve winner index, optional label, latency, kind, reason, and outcome with an explicit borrowed-vs-owned label model.
- [ ] Cancel losers through task controls only; no foreign mutation of ring/file/DB internals.
- [ ] Make cancellation reason explicit and observable.
- [ ] Make participant ownership explicit so races do not steal callbacks, miss callbacks, or orphan consumed loser handles.
- [ ] Make progress requirements explicit so cancel-and-wait cannot hang silently.
- [ ] Make rollback and cancellation paths safe when ready callbacks are already in flight.
- [ ] Provide ergonomic timeout/shutdown/disconnect wrappers on top of the same primitive.
- [ ] Keep performance available: small-N inline storage, owner-local timers, no forced OS timer per operation.
- [ ] Provide a polished example: race DB vs network vs disk and collect winner stats.

## Non-goals

- Do not replace every subsystem pump in one pass.
- Do not make bare `race` drive `io_uring` by itself.
- Do not hide operation-specific cancellation semantics.
- Do not guarantee that cancellation means the underlying syscall/query never happened.
- Do not make speculative duplicate side-effecting operations look safe.
- Do not remove existing `carrier::race(Chain<T>, Chain<T>)`.
- Do not support shared observation of one live task in Stage 1.
- Do not make labels mandatory; winner index is the authoritative identity.
- Do not allocate in bare `race` just to make labels safe. Dynamic-label safety belongs in an explicit owned wrapper.
- Do not support heterogeneous value races in Stage 1.
- Do not promise cross-owner outcome extraction without an owner-bound adapter.

## Core model

```cpp
using namespace conflux;
using namespace std::literals;

race_result<Blob> r = co_await race<Blob>(
    race_options{
        .winner = winner_policy::first_success,
        .losers = loser_policy::request_cancel_and_wait,
        .preserve_winner_latency = true,
    },
    candidate("db"sv,      db.fetch_blob(key)),
    candidate("network"sv, net.fetch_blob(key)),
    candidate("disk"sv,    disk.fetch_blob(key)),
    trigger("deadline"sv,  timeout_after(ring, 250ms), CancelReason::deadline),
    trigger("shutdown"sv,  server.shutdown_requested(), CancelReason::shutdown));
```

Meaning:

```text
participants are consumed by the race or are already-ready values
race exclusively owns each live participant's ready callback slot
winner policy decides which completion wins
winner identity and outcome are preserved
triggers can win with a cancellation/interruption reason
losers are handled according to loser_policy
every consumed loser remains owned until terminal completion is consumed, discarded, or transferred to an explicit detached sink
loser cancellation is requested through controls
owner-specific hooks perform actual cleanup
progress is supplied by the surrounding async owner/executor/pump
race itself is cancellable; external cancel cascades into live participants
```

## Public vocabulary

### `winner_policy`

```cpp
enum class winner_policy : std::uint8_t {
    first_completion,
    first_success,
};
```

#### `first_completion`

The first participant to complete wins, regardless of success/failure/cancel.

Best for:

```text
timeout wrappers
shutdown/drain races
operation-vs-peer-disconnect
true “any terminal state matters” orchestration
```

#### `first_success`

The first successful value candidate wins. Trigger winners still win immediately. Value candidate failures and cancellations are retained. If all value candidates fail/cancel and no trigger wins, the aggregate failure/cancel result is returned.

Best for:

```text
DB vs network vs disk first-responder benchmark
redundant read replicas
cache vs upstream when cache miss is represented as failure/cancel
```

The previous `first_value_or_all_failed` policy is removed from Stage 1 because it was behaviorally identical to the proposed `first_success`: both ignored early value failures, both allowed triggers to interrupt, and both returned aggregate failure when all values failed. If a later use case needs “soft triggers” or “deadline only after all candidates fail,” add a policy with distinct semantics and tests.

### `loser_policy`

```cpp
enum class loser_policy : std::uint8_t {
    leave_running,
    request_cancel,
    request_cancel_and_wait,
};
```

#### `leave_running`

Race returns the winner without requesting loser cancellation. For moved live participants, losers do **not** return to caller ownership; the race consumed them. Therefore `leave_running` must transfer each losing handle into a detached loser sink owned by race infrastructure until the loser reaches a terminal state and its outcome is discarded or recorded.

Allowed only when:

```text
losers do not borrow caller-owned memory that may be reused/freed before loser terminal completion
caller can tolerate duplicate work
side effects are absent or explicitly accepted
load amplification is budgeted
detached loser sink has a progress guarantee or cleanup budget
observability records late loser failure/cancel where enabled
```

Stage 1 should either implement the detached loser sink or reject `leave_running` for consumed live participants. It may still be valid for already-ready `Chain<T>` participants and future explicitly-shared/externally-owned participant adapters.

This is expert/speculation mode, not a default. It must never mean “drop the handle and hope someone else observes it.”

#### `request_cancel`

Race calls `request_cancel(reason)` on losers and returns after the winner without waiting for loser terminal completion.

For consumed live participants, this policy still cannot drop loser handles. A moved `TaskJoinHandle` remains live until terminal completion is consumed or explicitly abandoned, and its destructor terminates if it is destroyed live. Therefore `request_cancel` must transfer every consumed live loser into the same detached loser sink/abandon path used by `leave_running`, or the policy must be rejected for consumed live participants unless an external owner adapter owns terminal observation.

Allowed only when:

```text
loser owns all memory/resources needed until eventual completion
operation owner guarantees cleanup independently
caller does not need to know cleanup outcome
cancel is best-effort and late completion is acceptable
detached loser sink/abandon path owns consumed handles until terminal completion
observability records late loser failure/cancel where enabled
```

Unsafe for borrowed I/O unless the owner extends buffer/slot/lease lifetime until terminal completion. The default remains `request_cancel_and_wait` for borrowed operations.

#### `request_cancel_and_wait`

Race calls `request_cancel(reason)` on losers and waits until loser completion is observed/drained.

Required default for:

```text
borrowed socket buffers
borrowed file buffers
fixed-buffer slots visible to caller
direct-slot/fd lease operations
operations whose cleanup releases scarce owner resources
```

Not sufficient by itself:

```text
request_cancel_and_wait requires a progress-domain guarantee.
```

The surrounding context must keep making progress until all required losers terminate, or the race must be bound to an explicit owner/pump wrapper that does so. Bare `race` must not pretend it can complete an `io_uring` loser while no thread is pumping that ring.

### `loser_cleanup_policy`

```cpp
enum class loser_cleanup_policy : std::uint8_t {
    wait_unbounded,
    wait_until_cleanup_deadline,
    detach_after_cleanup_deadline,
    fail_after_cleanup_deadline,
};
```

`request_cancel_and_wait` can hang if an operation ignores cancellation. The API must make that visible.

For public server/drain use, configure a cleanup budget:

```cpp
race_options{
    .losers = loser_policy::request_cancel_and_wait,
    .cleanup = loser_cleanup_policy::fail_after_cleanup_deadline,
    .loser_cleanup_budget = 5s,
};
```

For borrowed I/O, `detach_after_cleanup_deadline` is unsafe unless the owner extends the borrowed memory lifetime. Default should be `fail_after_cleanup_deadline` or `wait_unbounded`, depending on call site.

### `race_options`

```cpp
struct race_options {
    winner_policy winner = winner_policy::first_completion;
    loser_policy losers = loser_policy::request_cancel_and_wait;
    loser_cleanup_policy cleanup = loser_cleanup_policy::wait_unbounded;
    std::chrono::steady_clock::duration loser_cleanup_budget{};
    CancelReason default_loser_reason = CancelReason::requested;
    bool preserve_winner_latency = false;
    bool collect_loser_outcomes = false;
};
```

Recommended defaults:

```text
internal borrowed I/O wrappers: first_completion + request_cancel_and_wait
public first-responder benchmark: first_success + request_cancel_and_wait
timeouts: first_completion + request_cancel_and_wait
shutdown/drain: first_completion + request_cancel_and_wait with explicit cleanup budget
speculative expert mode: caller may opt into leave_running
```

### `race_result<T>`

Winner identity is index-first. Labels are optional diagnostics. Bare `race` must not allocate just to preserve labels.

```cpp
enum class race_winner_kind : std::uint8_t {
    value_candidate,
    trigger,
};

struct race_winner_info {
    std::size_t index{};
    std::string_view label{}; // borrowed, optional
    race_winner_kind kind{};
    CancelReason reason = CancelReason::requested;
    std::chrono::nanoseconds latency{};
};

template<class T>
struct race_result {
    race_winner_info winner;
    root::Outcome<T> outcome;
};
```

The returned `winner.label` is valid only as long as the label storage supplied to the participant remains valid. This is intentional: labels are not part of race correctness, and the fast path should not allocate to protect a diagnostic string.

For a trigger winner in `race<T>`, the outcome is cancelled with the trigger reason:

```text
deadline trigger wins -> Outcome<T>{Cancelled{CancelReason::deadline}}
shutdown trigger wins -> Outcome<T>{Cancelled{CancelReason::shutdown}}
client_gone wins      -> Outcome<T>{Cancelled{CancelReason::requested}} plus diagnostic tag
```

This keeps Stage 1 same-typed. Later stages can add richer `race_interruption` metadata or heterogeneous winners.

### Owned-label convenience wrapper

Dynamic labels are supported by a separate wrapper function, not by an option bit on bare `race`. This makes the allocation boundary visible at the call site.

Minimal Stage 1 shape:

```cpp
template<class T>
struct owned_labeled_race_result {
    race_result<T> result;
    std::vector<std::string> labels;

    owned_labeled_race_result(owned_labeled_race_result const&) = delete;
    auto operator=(owned_labeled_race_result const&) -> owned_labeled_race_result& = delete;

    owned_labeled_race_result(owned_labeled_race_result&&) noexcept;
    auto operator=(owned_labeled_race_result&&) noexcept -> owned_labeled_race_result&;

    auto winner_label() const noexcept -> std::string_view;
};

template<class T, class... Participants>
auto race_owned_labels(race_options opts, Participants&&... ps)
    -> root::Task<owned_labeled_race_result<T>>;
```

`race_owned_labels` is a wrapper over bare `race`, with this contract:

```text
1. Collect participant count and borrowed labels.
2. Copy labels into wrapper-owned storage before any ready callback registration.
3. If label allocation/copy fails, no participant callback has been installed and no loser cancellation is needed.
4. Build internal borrowed-label participants whose std::string_view labels point into wrapper-owned storage.
5. Run bare race using the same registration, cancellation, loser-sink, progress, and capability rules.
6. Return a move-only owned_labeled_race_result whose result.winner.label points into labels.
7. Rebase winner.label on wrapper move; or make winner_label() the stable public accessor and keep result.winner.label best-effort/internal.
8. Setup-failure and aggregate-failure diagnostics emitted by this wrapper must use the copied labels, because the copy happened before registration.
```

Unlabeled participants get an empty owned label at their index. Index remains authoritative.

The wrapper may allocate once for the label vector and once per dynamic label string. That cost is explicit and opt-in. Bare `race` remains the normal zero-allocation label path.

## Participant labels

Participant labels are optional. Index is authoritative.

Stage 1 should accept the minimal ergonomic surface:

```cpp
candidate(task)              // no label, zero allocation
candidate("db"sv, task)      // borrowed label, zero allocation
trigger("deadline"sv, task, CancelReason::deadline)
```

Rules:

```text
Bare race stores labels as std::string_view only.
Bare race never copies labels.
String literals/static string_views are the recommended labeled fast path.
Dynamic labels must outlive race_result/aggregate diagnostics, or use race_owned_labels.
No fixed_string/UDL/small-label machinery is required in Stage 1.
```

This matches the project direction better than mandatory owned labels: make costs visible, allow documented footguns, keep the hot path allocation-free, and offer a safe owned wrapper for users who want convenience.

Aggregate errors follow the same rule: bare aggregate diagnostics contain indices and borrowed labels; owned-label wrapper errors own/carry labels for the exception lifetime.

## Participant ownership and registration contract

Current controls support one ready callback. A live race is therefore an exclusive observer.

### Rule: race consumes live participants

Stage 1 live participants must be moved into the race:

```cpp
template<class T>
auto candidate(race_name name, root::Task<T> task) -> race_candidate<T>;

template<class T>
auto candidate(race_name name, root::TaskJoinHandle<T> handle) -> race_candidate<T>;
```

The race owns the handle and is responsible for joining/taking its outcome exactly once. Passing only `TaskControl`, passing a borrowed handle reference, or observing a task already awaited elsewhere is not a Stage 1 API.

### Rule: registration uses `try_set_on_ready`, not `set_on_ready_or_run`

Race must call `control().try_set_on_ready(...)` and handle all statuses explicitly:

```text
installed         -> participant armed successfully
already_ready     -> run the participant's completion path immediately/inline
already_installed -> invalid shared participant; fail/assert and cancel/disarm already-armed participants
empty             -> invalid participant; fail/assert and cancel/disarm already-armed participants
```

Do not use `set_on_ready_or_run(...)` for race registration. It silently ignores `already_installed`, which could make a race hang or miss a participant.

### Rule: registration failure is atomic enough for users

If registration fails after some participants were armed:

```text
mark race state as setup_failed/aborted
clear/disarm armed callbacks where possible
if clear_on_ready() reports in_flight or already_terminal, keep race state alive until the callback path quiesces
request_cancel(default_loser_reason) on live participants already owned by race unless policy says otherwise
return a race setup failure or assert in debug for programmer error
never leave race state waiting forever
never destroy state captured by an installed callback before that callback can no longer run
```

`already_installed` is normally a programmer error because the participant was already observed. Public API should make this hard to express by accepting moved tasks/handles only.

Implementation rule: race state is heap-owned, and each installed ready callback captures a strong keepalive (`shared_ptr` or equivalent intrusive ref). Callback bodies must check an aborted flag before publishing completion. Rollback may return to the caller after publishing setup failure, but any in-flight callback must only touch still-live state and must become a no-op/cleanup path after seeing aborted state.

### Rule: callbacks must be nonblocking

A race ready callback should only mark race-local state and schedule/resume the race continuation. It must not call `blocking_join`. Outcome extraction should use ready-only APIs such as `join_ready(...)` or owner-bound equivalents once the participant is known ready.

A callback may run concurrently with setup rollback, external cancellation, or cleanup-budget expiry. It must therefore tolerate every race state phase: registering, active, winner_chosen, cancelling, draining, detached_loser_sink, setup_failed, and destroyed-by-user-but-kept-alive-by-callback.

## Capability and owner contract

A race may observe readiness through controls, but taking the outcome must obey existing capability rules.

Current root code already distinguishes unrestricted `Task<T>` from posted/operation work that requires a matching owner/driver capability. Race must not bypass that.

Stage 1 API:

```text
root::Task<T>
root::TaskJoinHandle<T>
already-ready carrier::Chain<T>
```

These are unbound from a progress capability from the race API's point of view.

Owner-bound participants should be a later, explicit API:

```cpp
template<class Owner, class T>
auto candidate_on(Owner& owner, race_name name, root::PostedJoinHandle<T> handle)
    -> owner_bound_race_candidate<T>;

template<class Driver, class T>
auto candidate_on(Driver& driver, race_name name, root::OperationJoinHandle<T> handle)
    -> owner_bound_race_candidate<T>;
```

Or expose owner-specific wrappers that convert owner-bound operations into ordinary `Task<T>` only after their owner owns the completion and outcome extraction path.

Cross-owner races are allowed only when each participant adapter owns its capability-safe extraction path. A generic race cannot steal outcomes from a posted/operation handle owned by a different capability.

## Progress-domain contract

Bare `race(...)` is an async coordination primitive. It does not drive kernel or subsystem progress.

Required guarantee for `co_await race(...)`:

```text
Every live participant has an active owner/executor/pump that will continue making progress until the participant completes or is cancelled and drained.
```

This is naturally true inside a running socket/server coroutine when the ring owner is alive. It is not naturally true inside a synchronous caller unless that caller pumps the relevant owner.

### Blocking/sync wrappers

Blocking APIs must bind race to a progress domain:

```cpp
template<class T>
T sync_wait_socket_race(
    SocketTaskRing& ring,
    root::Task<race_result<T>> race_task,
    std::optional<std::chrono::milliseconds> budget = std::nullopt);
```

This wrapper owns the socket ring pump exactly like `sync_wait_socket_task(...)` does today. Equivalent wrappers may exist for file/iopoll if they need owner-specific dispatch.

Do not document `request_cancel_and_wait` as safe in blocking contexts unless using an owner-bound sync wrapper or otherwise proving progress.

## Value candidates

```cpp
template<class T>
auto candidate(race_name name, root::Task<T> task) -> race_candidate<T>;

template<class T>
auto candidate(race_name name, root::TaskJoinHandle<T> handle) -> race_candidate<T>;

template<class T>
auto candidate(race_name name, carrier::Chain<T> chain) -> race_candidate<T>;
```

A current `carrier::Chain<T>` participant is already materialized. It is useful for cache hits and fast computed paths:

```cpp
auto r = co_await race<User>(
    race_options{.winner = winner_policy::first_success},
    candidate("memory"_race_name, memory_cache.lookup_chain(id)),
    candidate("db"_race_name, db.fetch_user(id)));
```

The `Chain<T>` path must not be described as live async racing. It is an already-ready participant.

## Triggers

Triggers are non-value participants. They win by producing an interruption reason.

```cpp
auto trigger(race_name name, root::Task<void> task, CancelReason reason) -> race_trigger;
auto trigger(race_name name, root::TaskJoinHandle<void> handle, CancelReason reason) -> race_trigger;
```

Convenience helpers:

```cpp
auto timeout_after(SocketTaskRing&, std::chrono::milliseconds) -> root::Task<void>;
auto timeout_at(SocketTaskRing&, std::chrono::steady_clock::time_point) -> root::Task<void>;
auto until_shutdown(Server&) -> race_trigger;
auto until_client_disconnect(RequestContext&) -> race_trigger;
auto until_drain_deadline(Server&, std::chrono::milliseconds) -> race_trigger;
auto until_backpressure_reject(Queue&) -> race_trigger;
auto until_stop_token(std::stop_token) -> race_trigger;
```

Timer helpers must be owner-local where possible:

```text
socket ring -> linked timeout or async_sleep_for
lane/runtime -> TimerService/timerfd heap
blocking fallback -> DeadlineScope/jthread only where appropriate
```

## Required root-layer change: cancellation reasons

Current `TaskControl::request_cancel()` is not enough. To make non-timeout triggers honest, cancellation reason must flow through the control layer and final cancelled outcomes.

Required API:

```cpp
class TaskControl {
public:
    bool request_cancel(CancelReason reason = CancelReason::requested) noexcept;
    [[nodiscard]] std::optional<CancelReason> cancellation_reason() const noexcept;
};
```

Required behavior:

```text
first cancel request wins and stores reason
stop_source request_stop still fires
installed cancel hook receives stored reason
late-installed cancel hook receives stored reason if already cancel-requested
terminal cancelled outcome uses stored reason when operation cooperatively acknowledges cancel
old request_cancel() call sites remain valid and mean requested
```

`TaskSource<T>` should expose reasonful cancellation directly:

```cpp
bool try_set_cancelled(CancelReason reason) noexcept;
bool try_set_cancelled(work_errc errc = work_errc::cancelled_requested) noexcept;
```

`Scope::cancel(reason)` should forward the reason:

```cpp
for (auto& c : task)   c.request_cancel(reason);
for (auto& c : posted) c.request_cancel(reason);
for (auto& c : op)     c.request_cancel(reason);
```

`join_all`, future race, timeout wrappers, drain wrappers, and client cancellation relay should use the configured reason when cancelling siblings or active children.

### Cancel-hook audit requirement

Stage 1 is not just changing `TaskControl::request_cancel(reason)`. It must audit and update cancellation hooks and cancellation commits.

For each `install_cancel_hook([](CancelReason reason) { ... })`:

- If the operation commits `Cancelled`, pass the received `reason` to `try_set_cancelled(reason)`.
- If the operation converts cancellation into a domain-specific exception, preserve the reason in the exception or diagnostic where possible.
- If the operation cannot represent the reason, document the collapse to `requested` and add a test.
- If the hook submits an owner-thread cancel SQE and the eventual CQE returns `-ECANCELED`, the stored/requested reason must still be available when committing the final cancelled outcome.

Important current areas to audit:

```text
src/socket_io/socket_io_coro_impl.cxx  send/recv/write_all/read_some/sleep cancel hooks
src/net/client_async_impl.cxx          active request cancellation relay
src/net/dns/dns_impl.cxx               DNS query/coalescing cancellation
src/db/pool.cxx                        queued acquire cancellation
src/db/connection.cxx                  query/transaction cancellation
src/work_api.cxx                       join_all sibling cancellation
src/work/carrier_scope.cxx             Scope cancellation
src/net/cancel_impl.cxx                HTTP/server-side cancellation helpers
```

## Failure aggregation

For same-type value candidates under `first_success`:

```text
one failure, all others cancelled/missing -> return that failure
multiple failures -> AggregateError with per-participant labels
all cancelled -> return first cancellation, preserving reason
trigger wins -> return trigger cancellation reason
```

Race failure diagnostics should include participant labels and indices.

Proposed diagnostic shape:

```cpp
class race_aggregate_error : public std::exception {
public:
    struct entry {
        std::size_t index;
        std::string_view label; // borrowed for bare race; owned wrapper uses copied label storage
        std::exception_ptr error;
    };

    std::span<entry const> entries() const noexcept;
};
```

Bare `race_aggregate_error` owns its entries but borrows labels under the same lifetime rule as `race_result`. The owned-label wrapper may throw/return an owned variant whose labels remain valid for the exception/result lifetime. Indexes are always valid and authoritative.

## Nested races

Nested race must be structured:

```text
outer task cancel -> active inner race cancel -> inner children request_cancel(reason)
```

Example:

```cpp
Task<Response> handler(Request req) {
    auto user = co_await value_or_throw(co_await race<User>(
        race_options{.winner = winner_policy::first_success},
        candidate("memory"_race_name, memory_cache.lookup(req.user_id())),
        candidate("disk"_race_name, disk_cache.lookup(req.user_id())),
        candidate("db"_race_name, db.lookup_user(req.user_id())),
        until_client_disconnect(req)));

    auto details = co_await value_or_throw(co_await race<Details>(
        race_options{},
        candidate("local"_race_name, local_details(user.id)),
        candidate("remote"_race_name, remote_details(user.id)),
        trigger("deadline"_race_name, timeout_after(req.ring(), 50ms), CancelReason::deadline)));

    co_return http::json(make_response(user, details));
}
```

No nested race may detach children by accident. Detach requires explicit `leave_running`.

## HTTP server use

HTTP should expose named config because labels describe security and UX intent:

```text
header_timeout
body_idle_timeout
write_timeout
keep_alive_idle_timeout
drain_deadline
```

Internally those should map to race participants:

```cpp
auto headers = co_await race<HeaderBlock>(
    race_options{},
    candidate("read_headers"_race_name, read_headers(conn)),
    trigger("header_timeout"_race_name, timeout_after(ring, cfg.header_timeout), CancelReason::deadline),
    until_shutdown(server));

auto body = co_await race<Body>(
    race_options{},
    candidate("read_body"_race_name, read_body(conn)),
    trigger("body_idle_timeout"_race_name, timeout_after(ring, cfg.body_idle_timeout), CancelReason::deadline),
    until_client_disconnect(conn));
```

Avoid hidden total request timeout by default:

```cpp
cfg.total_request_timeout = disabled;
```

Normal uploads should not fail merely because total elapsed time grew while bytes kept making progress.

For blocking HTTP/socket utilities, expose owner-bound wrappers that pump the ring; do not use bare `race` from a synchronous context.

## User-facing example: first-responder benchmark

This should be an official example once the primitive exists.

```cpp
import conflux.http;
import conflux.work.race;

using namespace std::chrono_literals;

struct SourceStats {
    std::uint64_t wins = 0;
    std::uint64_t failures = 0;
    std::chrono::nanoseconds total_latency{};
};

Task<void> benchmark_sources(Keys keys, Db& db, Client& net, DiskCache& disk, SocketTaskRing& ring) {
    std::unordered_map<std::string, SourceStats> stats;

    for (auto const& key : keys) {
        auto r = co_await race<Blob>(
            race_options{
                .winner = winner_policy::first_success,
                .losers = loser_policy::request_cancel_and_wait,
                .preserve_winner_latency = true,
            },
            candidate("db"sv,      db.fetch_blob(key)),
            candidate("network"sv, net.fetch_blob(key)),
            candidate("disk"sv,    disk.fetch_blob(key)),
            trigger("deadline"sv,  timeout_after(ring, 250ms), CancelReason::deadline));

        auto& s = stats[std::string{r.winner.label}];
        if (r.outcome.is_success()) {
            ++s.wins;
            s.total_latency += r.winner.latency;
        } else {
            ++s.failures;
        }
    }

    for (auto const& [name, s] : stats) {
        auto avg = s.wins == 0 ? 0 : s.total_latency.count() / s.wins;
        println("{} wins={} failures={} avg={}ns", name, s.wins, s.failures, avg);
    }
}
```

Decision-tree use:

```text
A keys -> network first
B keys -> DB first
C keys -> local disk cache first
```

Documentation caveats:

- Only use read-only/idempotent operations unless duplicate side effects are acceptable.
- Speculative racing amplifies load across DB/network/disk.
- Winner rate alone is insufficient; also track cost, failure rate, tail latency, and backend load.
- Production speculative racing must have concurrency budgets and backpressure.

## Observability

Race should optionally emit:

```text
race.label
race.participant_count
winner.label
winner.kind
winner.reason
winner.latency
loser.cancel_requested count
loser.cleanup_latency histogram
loser.cleanup_timeout count
registration_failed count
already_installed count
all_failed count
trigger_won count
capability_mismatch count
progress_timeout count
```

This makes race useful both as an internal primitive and a user-side decision tool.

## Performance constraints

- Small N should use inline storage, not per-participant heap nodes.
- Stage 1 should optimize N <= 4 because common races are op+timer, op+timer+shutdown, DB+network+disk+deadline.
- Race state should allocate once at most.
- Already-ready `Chain<T>` participants should be handled without scheduling.
- Registration should avoid per-participant heap allocation for common small N.
- Timer backend should be owner-local:
  - socket linked timeout or existing `async_sleep_for` for socket rings;
  - `TimerService`/timerfd heap for lane-local timers;
  - `DeadlineScope`/`jthread` only for blocking fallback paths.
- Do not force one OS timer per tiny operation if a batched timer service is available.
- Preserve existing linked-timeout socket implementation internally if benchmarks show it is materially faster than generic race.
- Do not use `blocking_join` inside ready callbacks.
- Any performance-motivated change must be benchmarked in optimized builds with representative workloads before claiming a win.

## Safety constraints

1. Race never mutates owner internals directly.
2. Cancellation goes through `request_cancel(reason)`.
3. Owner cancel hooks decide where cleanup runs.
4. Borrowed I/O defaults to cancel-and-wait.
5. Cancellation is best effort unless operation docs promise more.
6. Loser terminal completion must be observed when cleanup releases owner resources.
7. `request_cancel_and_wait` requires active progress.
8. Race consumes live participants and owns their ready callback slot.
9. Race must fail/assert on `already_installed`; it must not silently ignore registration failure.
10. Outcome extraction must respect `can_join_with(...)` / `required_capability()`.
11. Cross-owner races are allowed only through owner-bound adapters.
12. Nested races are structured.
13. Speculative racing requires budgets.
14. Bare `race` labels are borrowed, optional, and zero-allocation; dynamic labels require caller lifetime discipline or `race_owned_labels`.
15. Detaching losers that borrow memory is unsafe unless lifetime is extended by the owner.
16. `leave_running` for consumed live participants requires a detached loser sink; dropping the handle is invalid.
17. `request_cancel` for consumed live participants also requires a detached loser sink/abandon path unless an external owner adapter observes terminal completion.
18. Rollback/cancel paths must keep race state alive until in-flight callbacks quiesce.

## Final review resolutions

### R1: label lifetime and allocation

Verified issue: returning `std::string_view` labels is unsafe if the race copied labels into transient race state and then destroyed that state before returning the result.

Resolution: do not copy labels in bare `race`. Labels are optional borrowed diagnostics, and winner index is authoritative. Bare `race` is zero-allocation for labels. Users should pass static/string-literal labels (`"db"sv`) or ensure dynamic label storage outlives the result/exception. A separate `race_owned_labels` wrapper copies labels and returns a wrapper whose winning label view points into owned storage.

Rationale: mandatory owned labels optimize for preventing a diagnostic footgun at the cost of hidden allocation in every labeled race. That conflicts with Conflux's cost-transparent API direction. The safe convenience path exists, but the fast path stays explicit.

### R2: rollback and in-flight callback lifetime

Verified issue: `clear_on_ready()` has an `in_flight` state. A rollback path that destroys race state after “clear where possible” can race with an already-running ready callback.

Resolution: every installed ready callback owns a keepalive to race state and checks an aborted/setup_failed flag before publishing completion. Rollback records failure, disarms what it can, cancels or abandons owned participants as required, and lets in-flight callbacks quiesce against still-live state.

### R3: `leave_running` with consumed participants

Verified issue: Stage 1 consumes moved handles, so losers cannot continue “independently” with another observer unless an explicit adapter supplies one.

Resolution: for consumed live participants, `leave_running` transfers losers into a detached loser sink that owns handles until terminal completion and discards/records outcomes. If that sink is not implemented, `leave_running` is rejected for consumed live participants. This keeps exclusive ownership and avoids leaks/orphaned terminal outcomes.

### R4: `request_cancel` with consumed participants

Verified issue: `request_cancel` returns before loser terminal completion, but consumed loser handles are still live. Destroying those handles is invalid because live `TaskJoinHandle` destruction terminates. The race also owns the callback slot, so late completion needs a still-live observation path.

Resolution: for consumed live participants, `request_cancel` transfers losers into the same detached loser sink/abandon path used by `leave_running`, after requesting cancellation. If that sink is not implemented, reject `request_cancel` for consumed live participants unless an owner-bound/external adapter owns terminal observation. Borrowed I/O should prefer `request_cancel_and_wait`.

## API surface placement

Recommended modules:

```text
conflux.work.race          stable candidate once implemented
conflux.work.root          low-level task/control substrate
conflux.work.carrier       existing value-level chain helpers
conflux.http               re-export ergonomic race wrappers only if needed
conflux.runtime            advanced timers / owner-local trigger helpers
conflux.socket_io.blocking owner-bound sync wait wrappers, not bare race
```

Keep raw ring-specific machinery out of curated `import conflux;` unless wrapped by safe helpers.

## Migration plan

### Stage 0 — document and test current behavior

- Add tests showing `request_cancel()` currently reports `requested`.
- Add tests showing late cancel hooks currently receive `requested`.
- Add tests for current `carrier::race(Chain<T>, Chain<T>)` semantics.
- Add docs saying current carrier race is outcome-level, not live async race.
- Add root tests documenting `try_set_on_ready` one-callback semantics.
- Add regression test proving `set_on_ready_or_run` ignores `already_installed`; this is a reason not to use it in race.
- Add tests showing capability mismatch is enforced by join helpers.

### Stage 1 — cancellation reason propagation and hook audit

- Change `TaskControl::request_cancel()` to `request_cancel(CancelReason reason = requested)`.
- Store first cancellation reason in the control block.
- Add `TaskControl::cancellation_reason()` or equivalent.
- Invoke cancel hooks with stored reason.
- Late-installed cancel hooks receive stored reason.
- Add public `TaskSource<T>::try_set_cancelled(CancelReason)` overloads.
- Update `Scope::cancel(reason)` to forward reason.
- Update `join_all` sibling cancellation to use configured/default reason.
- Audit current cancel hooks and update default `try_set_cancelled()` call sites where reason should propagate.
- Keep all old call sites valid.

### Stage 2 — result labels, callback lifetime, and same-type live N-way race for unbound tasks

- Add `race_options`, `winner_policy`, `loser_policy`, `race_result<T>`, and optional `owned_labeled_race_result<T>` wrapper.
- Add same-type `race<T>(opts, participants...)`.
- Support value candidates and void triggers.
- Support moved `Task<T>`, moved `TaskJoinHandle<T>`, and already-ready `Chain<T>` participants.
- Use `try_set_on_ready`, not `set_on_ready_or_run`.
- Fail/assert on `already_installed` or `empty` participants.
- Keep bare labels borrowed/zero-allocation; add `race_owned_labels` for result-owned dynamic labels.
- Ensure installed callbacks hold race state alive until quiesced, including setup rollback.
- For `leave_running` and `request_cancel`, implement detached loser sink/abandon path or reject those policies for consumed live participants.
- Default to small-N inline participant state.
- Do not support owner-bound posted/operation handles yet except through explicit wrappers that convert them safely.

### Stage 3 — progress-bound wrappers and trigger helpers

- Add `with_timeout(op, timer)` as a thin wrapper.
- Add `timeout_after(...)` / `timeout_at(...)` wrappers for socket owner first.
- Add shutdown/disconnect/drain/stop-token trigger helpers.
- Add blocking owner-bound wrappers such as `sync_wait_socket_race(...)` where needed.
- Reimplement socket timeout overloads in terms of race only if benchmarks show the generic path is not materially worse; otherwise keep linked-timeout implementation as optimized backend with race-equivalent semantics.

### Stage 4 — owner/capability-bound participants

- Add `candidate_on(owner, ...)` for posted work only if outcome extraction can run in the owner/capability context.
- Add `candidate_on(driver, ...)` for operation work only if outcome extraction can run in the driver/capability context.
- Add tests for capability mismatch, wrong-owner extraction, and cross-owner cancellation.
- Keep cross-owner races limited to adapters that own their safe extraction path.

### Stage 5 — HTTP/server adoption

- Convert header/body/write/drain paths where feasible.
- Keep coarse sweep only for idle connection cleanup if cheaper.
- Remove hidden total request timeout from defaults or rename it explicitly.
- Add rejection diagnostics and metrics for trigger winners.
- Add slow-header/header-timeout race tests.
- Add body-idle timeout tests proving uploads making progress are not killed by hidden total timeout.

### Stage 6 — file/iopoll/DB adoption

- Add file/iopoll only after cancellation hooks safely release slots/buffers and progress wrappers exist.
- Add DB pool/query cancellation once backend semantics are explicit.
- Add pressure tests for DB pool exhaustion and queued acquire cancellation.
- Add tests proving queued acquire cancellation does not leak waiters.

### Stage 7 — user-facing examples and docs

- Add first-responder benchmark example.
- Add docs explaining when speculative racing is good or bad.
- Add cancellation guarantee table for raced operation families.
- Add cost/alloc/block badges for race helpers.
- Add observability examples for winner distribution and cleanup timeouts.

## Tests to add

### Root/control tests

- `request_cancel(deadline)` stores and forwards deadline.
- First cancel reason wins.
- `cancellation_reason()` returns stored reason after first request.
- Late cancel hook receives stored reason.
- Cancel hook that commits cancellation can call `try_set_cancelled(reason)`.
- `Scope::cancel(shutdown)` forwards shutdown.
- `join_all` sibling cancellation can forward configured reason.
- Moved/empty controls remain safe.
- `try_set_on_ready` second registration returns `already_installed`.
- Race registration helper fails/asserts on `already_installed`.
- Rollback after partial registration is safe when `clear_on_ready()` reports `in_flight`.
- In-flight ready callback after setup failure sees aborted state and does not use freed memory.

### Race tests

- N-way first completion wins.
- N-way first success ignores early failure and later success wins.
- Fast value failure does not win under `first_success` while another value candidate can still succeed.
- Trigger wins immediately under both policies.
- All value candidates fail under bare `race` -> aggregate failure includes indices and valid borrowed/static labels when supplied.
- Static/literal `race_result::winner.label` remains valid after race state destruction.
- `race_owned_labels` with dynamic labels -> `race_result::winner.label`, `winner_label()`, and aggregate diagnostics remain valid for the owned wrapper/error lifetime.
- Deadline trigger wins -> cancelled deadline.
- Shutdown trigger wins -> cancelled shutdown.
- External race cancellation cancels all live participants with the external reason.
- `leave_running` does not cancel losers and keeps consumed loser handles in detached loser sink until terminal completion.
- `request_cancel` requests cancel, returns without waiting, and still keeps consumed loser handles in detached loser sink until terminal completion.
- `request_cancel_and_wait` waits for loser terminal completion when progress exists.
- Cleanup budget fires when loser ignores cancel.
- Already-ready `Chain<T>` can beat live task.
- `already_installed` participant causes setup failure instead of hang.
- Empty/moved participant causes setup failure instead of hang.
- Cross-owner participants require owner-bound adapter.
- Capability mismatch is diagnosed.
- Nested race outer cancel cancels inner race and children.

### Progress-domain tests

- Bare race does not claim to pump `io_uring`.
- `sync_wait_socket_race(...)` pumps CQEs until winner and loser cleanup complete.
- `request_cancel_and_wait` in a no-progress fake domain hits cleanup budget instead of hanging forever.
- Blocking wrapper cancellation preserves reason.

### Socket/file/server tests

- Borrowed recv timeout does not return until kernel no longer owns buffer.
- Socket cancel hook receives deadline when timer wins.
- Socket sleep cancelled by deadline returns `Cancelled{deadline}`, not default requested.
- Cross-thread socket cancellation goes through owner marshal.
- HTTP slow header triggers header-timeout race.
- HTTP body making progress is not killed by hidden total timeout.
- Drain races active work against drain deadline and emits metrics.
- DB queued acquire cancellation does not leak pool waiters.

### Example tests

- First-responder benchmark compiles.
- Example records winner distribution.
- Loser tasks do not leak.
- Docs warn about speculative duplicate load.

## Open questions

1. Should value-candidate cancellation under `first_success` be treated as failure or as immediate interruption?
   - Recommendation: value-candidate cancellation is retained like failure; triggers interrupt immediately.
2. Should participant labels be owned by default?
   - Recommendation: no. Bare `race` uses borrowed `std::string_view` labels or no labels. `race_owned_labels` is the convenience wrapper for dynamic labels.
3. Should Stage 1 expose heterogeneous race?
   - Recommendation: no. Require common `T`; add variant/typed winner later.
4. Should loser cleanup budget be part of `race_options` or only drain/server wrappers?
   - Recommendation: part of `race_options`; wrappers provide defaults.
5. Should `with_timeout` return `Task<T>` or `Task<race_result<T>>`?
   - Recommendation: both. `with_timeout` returns `Task<T>` for ergonomics; `race` returns `race_result<T>` for introspection.
6. Should existing socket linked timeouts be deleted?
   - Recommendation: no. Keep as optimized backend if benchmarked faster; public semantics should still be race semantics.
7. Should Stage 1 accept `TaskJoinHandle<T>` or only `Task<T>`?
   - Recommendation: accept moved `TaskJoinHandle<T>` only if registration can prove it is unobserved; otherwise start with `Task<T>` plus `Chain<T>` and add join handles after tests.
8. Should owner-bound posted/operation participants be part of the initial public API?
   - Recommendation: no. Add them only after a capability-bound adapter design is tested.
9. Should Stage 1 expose `leave_running` publicly?
   - Recommendation: expose only if detached loser sink is implemented and tested; otherwise keep enum internal/deferred and use `request_cancel_and_wait` defaults.

## Final recommendation

Implement this, but in the staged order above.

The concept is sound:

```text
one primitive: N-way race
many participants: value task, ready chain, timeout, shutdown, disconnect, drain, backpressure
explicit winner policy: first completion vs first success
explicit loser policy: leave via detached sink, cancel via detached sink, cancel-and-wait
reasonful cancellation: request_cancel(reason)
owner-safe cleanup: cancel hooks own subsystem details
progress-safe waiting: cancel-and-wait only with active progress
exclusive registration: moved participants only; no shared ready callback stealing
capability-safe extraction: outcome joins obey owner/driver capability
user-visible examples: DB vs network vs disk first-responder benchmark
```

This is smoother than operation-specific timeout parameters, more composable than a timeout subsystem, and safer than a generic pump that mutates foreign owners.

The remaining risk is implementation discipline, not direction. The active
implementation checklist is:

- [ ] Root-layer reason propagation.
- [ ] Ready-callback exclusivity tests.
- [ ] Callback lifetime/quiescence tests.
- [ ] Progress-domain assumption tests.
- [ ] Capability-safe join/extraction tests.
- [ ] Live N-way race after the root contracts are locked.
