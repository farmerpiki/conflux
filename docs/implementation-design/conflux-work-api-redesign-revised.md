# `conflux.work` API Redesign (Revised)

Status: revised draft

This document supersedes the previous replacement draft in
[conflux-work-api-redesign.md](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md).

The locked pre-plan decisions are recorded in
[conflux-work-api-redesign-decisions.md](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign-decisions.md).

## Summary

The redesign keeps conflux's execution models explicit instead of forcing them
behind one generic async type.

The revised direction is:

- three root execution categories, not two
- synchronous rejection at admission
- explicit affinity and explicit owner-driven execution
- best-effort cancellation with conservative aggregate semantics
- no hidden helper runtime
- no generic droppable async result in core work
- coroutine/composition mechanism deferred until benchmarked

## Core Decisions

1. `conflux.work` has three root categories:
   - autonomous scheduled work
   - explicitly posted owner work
   - driver / CQE / state-machine operations
2. Rejection is synchronous at the admission boundary.
3. Accepted root async values terminate only as success, failure, or
   cancellation.
4. Every accepted root async value owns one intrusive shared control block.
5. Result objects are linear. Long-lived background work uses explicit
   handle-first start APIs, not ordinary result dropping.
6. Cancellation is explicit and best-effort.
7. Affinity is part of the public contract.
8. Owner-driven execution remains explicit. There is no hidden helper runtime.
9. Droppable / coalescing stream primitives are intentionally omitted from the
   core redesign for now.

## Root Categories

This document uses working names for the three categories. Exact public names
can still change, but the split is locked.

### `Task<T>`

`Task<T>` is autonomous scheduled work.

Use it for:

- `WorkPool` jobs
- CPU work
- blocking adapters running on worker threads
- completions that can resolve without a specific owner pumping

`Task<T>` supports:

- `wait(task)`
- `get(task)`
- `co_await task` in compatible coroutine context

### `Posted<T>`

`Posted<T>` is explicitly posted owner work.

Use it for:

- `RingLane` queue-node work
- owner-thread closures that must execute on the owner
- single-issuer posted work where the owner drains the queue

`Posted<T>` supports:

- `block_on(owner, posted)`
- `get(owner, posted)`
- `co_await posted` in compatible owner-bound coroutine context

`Posted<T>` does not support plain `wait(posted)`.

### `Operation<T>`

`Operation<T>` is a driver / CQE / state-machine operation.

Use it for:

- `file_io`
- `db`
- other externally driven operations where completion depends on a driver,
  CQE path, or owner-owned state machine

`Operation<T>` supports:

- `block_on(owner, op)`
- `get(owner, op)`
- `co_await op` in compatible owner-bound coroutine context

`Operation<T>` does not support plain `wait(op)`.

### Why `Posted<T>` And `Operation<T>` Are Separate

Both are owner-driven, but they are not the same shape.

`Posted<T>` is queued user work that executes on the owner.

`Operation<T>` is not a posted closure. It is an owner-visible operation whose
progress is driven by CQEs, polling phases, or subsystem state transitions.

The common `Operation<T>` contract is intentionally narrower than the previous
draft's owner-driven category. Core work standardizes only:

- admission / rejection
- terminal outcome
- cancellation request
- owner / driver affinity
- owner-driven blocking and awaiting

It does not claim identical intermediate state, snapshot shape, or
subsystem-specific cancellation mechanics across `file_io`, `db`, and future
driver-owned subsystems.

## Admission, Rejection, And Outcomes

Admission is synchronous.

If admission fails, no async value exists and the caller gets `Reject`.

If admission succeeds, the async value exists and can only finish as:

- success
- failure
- cancelled

There is no rejected terminal state for an already-admitted async value.

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
        resource_exhausted,
        wake_failed,
    } reason;
};

template<class T>
using Success = std::conditional_t<std::is_void_v<T>, ok_t, T>;

template<class T>
using Outcome = std::variant<Success<T>, Failure, Cancelled>;

} // namespace conflux::work
```

### Admission Surfaces

The admission boundary must be uniform across subsystems.

Working shape:

```cpp
template<class Exec, class Fn>
auto submit(Exec& exec, Fn&& fn)
    -> std::expected<Task<submit_result_t<Fn>>, Reject>;

template<class Owner, class Fn>
auto post(Owner& owner, Fn&& fn)
    -> std::expected<Posted<post_result_t<Fn>>, Reject>;
```

Subsystem-owned operations follow the same rule. If they can fail before the
operation is armed, queued, or registered with its owner/driver, that failure
is synchronous `Reject`, not asynchronous `Failure`.

Examples:

- `file_io` submission-resource failures such as "cannot arm this operation
  now" are rejection
- `db` admission failures before a query operation exists are rejection
- protocol, SQL, kernel, or remote failures after admission are failure

`process` remains a domain-result-heavy API. Admission rejection only covers
work-runtime admission. Process-domain failures remain payload-level results,
for example `Task<expected<RunResult, error_code>>`.

## Ownership, Handles, And Lifetime

Every accepted root async value owns one intrusive shared control block.

That control block carries:

- intrusive reference count
- terminal outcome storage
- cancellation request state
- category-specific waiter / completion state
- minimal observation state

The hard contract is one control block per accepted root async boundary.

The redesign does not yet promise a specific zero-allocation story for every
derived combinator stage. That remains implementation work and must be
validated by benchmarks.

### Linear Result Objects

Root result objects are linear and move-only.

A live result object must be exactly one of:

- awaited
- consumed via `wait/get` or `block_on/get(owner, ...)`
- moved elsewhere
- handed to an explicit background-start API

Destroying a live result object without doing one of those is a program error.

### Handles

Control handles are copyable and own cancellation / observation rights.

Working shapes:

- `TaskHandle`
- `PostedHandle`
- `OperationHandle`

Handles support:

- `request_cancel(...)`
- `cancel_requested()`
- terminal-ready / minimal snapshot observation

### Background Work

The redesign does not treat `detach(result)` as the normal server pattern.

Long-lived request, connection, or session work should use explicit
handle-returning start APIs so background lifetime is admitted intentionally.

Working shape:

```cpp
template<class Exec, class Fn>
auto spawn(Exec& exec, Fn&& fn)
    -> std::expected<TaskHandle, Reject>;

template<class Owner, class Fn>
auto spawn(Owner& owner, Fn&& fn)
    -> std::expected<PostedHandle, Reject>;
```

Exact naming is not locked, but the design requirement is:

- background lifetime must be explicit
- stored handles, not ordinary result objects, are the normal ownership token
  for long-lived server flows

Equivalent handle-first start surfaces may also be needed for operation-backed
or mixed-category chains once the final carrier model is chosen.

## Blocking And Awaiting

```cpp
template<class T>
Outcome<T> wait(Task<T>&& task);

template<class T>
Success<T> get(Task<T>&& task); // throws on failure / cancel

template<class Owner, class T>
Outcome<T> block_on(Owner& owner, Posted<T>&& posted);

template<class Owner, class T>
Outcome<T> block_on(Owner& owner, Operation<T>&& op);

template<class Owner, class T>
Success<T> get(Owner& owner, Posted<T>&& posted); // throws on failure / cancel

template<class Owner, class T>
Success<T> get(Owner& owner, Operation<T>&& op); // throws on failure / cancel
```

`block_on(owner, ...)` is not an optional helper. It is part of the category
contract for owner-driven work.

## Affinity And Resumption

Affinity is a semantic contract, not an implementation detail.

### Locked Direction

Critical owner work keeps hard affinity.

Longer-running coroutine work may later support an explicit migratable policy,
but only inside an allowed resume domain. "May hop anywhere" is not an
acceptable default.

The working semantic split is:

- `pinned_owner`: resume only on the designated owner
- `migratable(domain)`: may resume only inside an explicitly allowed long-
  running domain

Admission or spawn can choose the initial policy, but that does not remove the
need for explicit continuation rules. The library must still define where
`co_await` and continuation callbacks run.

### Default Resumption Rules

The default rule is:

- continuation callbacks run on the completion context of their source
- `pinned_owner` sources resume on their owner
- autonomous scheduled sources resume on the context that commits completion,
  unless an explicit hop changes that

This applies both to raw `co_await` and to combinator callbacks.

### Diagnostics And Timing

Performance intent may be annotated separately from affinity semantics.

Possible future annotations:

- `critical`
- `heapless_expected`
- explicit time budget on spawn / admission

These are not semantic dispatch rules.

Deferred instrumentation idea:

- optional compile-time-gated timing instrumentation
- optional time budget parameter on spawn / admission
- optional release-build availability for users who want it
- customizable thresholds

The intended use is to catch latency-sensitive coroutine work that exceeds its
budget. Coroutine-frame allocation status can be treated as a weak hint, but
not as the primary policy signal.

## Coroutine And Composition Contract

The external contract is locked before the implementation mechanism is locked.

What is locked:

- mixed async chains must remain expressible
- hot owner paths must not be forced into a hop-friendly model
- there is no hidden helper runtime
- affinity changes must be explicit in the public model

What is still deferred pending implementation spikes and benchmarks:

- whether combinators and coroutine returns live directly on the root
  categories
- whether there is a generic composition / coroutine carrier above the root
  categories

This document therefore fixes semantics, not the final carrier shape.

Whatever carrier model wins, it must preserve these rules:

- the admission boundary is explicit and typed
- owner affinity is not silently weakened by continuation chaining
- combinator callback affinity is defined, not incidental
- mixed chains used by TLS / protocol / driver code remain viable without a
  hidden scheduler

## Cancellation And Aggregation

Cancellation is explicit and best-effort.

`request_cancel(reason)` does exactly this:

1. records the first cancel request
2. arranges producer notification if the category supports it
3. does not itself guarantee prompt terminal completion

### Category Semantics

For `Task<T>`:

- cancellation before start may let the executor skip the work
- once running, cancellation is cooperative

For `Posted<T>`:

- foreign-thread cancellation does not perform queue surgery
- the owner observes cancelled posted work at the next safe owner-visible point
- if not yet started, the owner can complete it as cancelled without running
  user code
- if already running, cancellation is cooperative

For `Operation<T>`:

- cancellation may trigger category-specific producer hooks
- hooks may attempt kernel, remote, or state-machine cancellation
- completion still depends on the subsystem's real drive model
- owner / driver progress may still be required before terminal cancellation is
  observed

### Aggregates

Generic aggregates must not assume prompt sibling termination after
`request_cancel()`.

Locked rule:

- `when_all(...)` waits for all children to become terminal

This is the default because it is the only generic contract that does not
pretend sibling cancellation is prompt.

Fail-fast forms may exist later as separate APIs. If added, they may request
sibling cancellation, but they must document that siblings can keep running for
some time or indefinitely.

`race(...)`-style APIs may still complete on the first terminal child, but they
must treat loser cancellation as best-effort and be implemented so late
completions are safely absorbed.

## Subsystem Mapping

### `WorkPool`

Accepted work becomes `Task<T>`.

Admission rejection covers:

- `stopped`
- `queue_full`

### `RingLane`

Posted owner work becomes `Posted<T>`.

Admission rejection covers:

- `stopped`
- `queue_full`
- `wake_failed`

### `file_io`

`file_io` operations become `Operation<T>`.

The important change from the previous draft is that pre-admission submission
failures are rejection, not async failure.

The common `Operation<T>` contract does not try to hide `file_io`'s specific
CQE-slot, stale-generation, or multishot details. Those remain subsystem
semantics layered on top of the shared work contract.

### `db`

`db` query / poll operations become `Operation<T>`.

The common `Operation<T>` contract does not claim that libpq-state-machine
driving looks identical to `file_io`. It standardizes only admission,
cancellation request, outcome, and owner-affine completion.

### `process`

`process` remains autonomous scheduled work.

The recommended shape remains domain-result-oriented where that matches the API
best:

- `Task<expected<Process, error_code>>`
- `Task<expected<RunResult, error_code>>`

### TLS / Protocol Coroutines

TLS and protocol code must remain able to express mixed chains that involve
driver-owned operations and autonomous work.

The exact carrier model is deferred, but the redesign must not force these
paths into brittle bridge boilerplate or hidden runtime hops.

### `router` / `http_server`

Long-lived deferred-response and connection work should store explicit handles
returned by background-start APIs.

The redesign should not rely on "create result object, extract handle, then
`detach(...)`" as the normal ownership path.

## Performance Targets

The design is only acceptable if implementation work preserves these targets:

- one intrusive control block per accepted root async boundary
- move-only result objects
- copyable intrusive handles
- no `std::shared_ptr`
- bounded queues
- no hidden helper runtime
- no hidden owner-driving thread
- no foreign-thread queue surgery for owner work

These are design targets, not blanket promises about every intermediate
continuation node until the implementation proves them.

## Intentionally Deferred Or Omitted

Deferred:

- final coroutine / combinator carrier model
- optional timing instrumentation and per-spawn budgets
- exact public naming of the three root categories

Intentionally omitted for now:

- generic droppable async result objects
- generic coalescing / latest-wins stream primitives in core work
- automatic affinity semantics derived from coroutine-frame allocation shape
