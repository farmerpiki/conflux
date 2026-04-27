# `conflux.work` Continuation Plan — Phases 5–10

Status: planning — covers all items deferred by V10 and the working decision log.

Source alignment:

- `conflux-work-api-redesign-proposal-v10.md` §Explicitly Deferred, §Post-Implementation Evaluation
- `conflux-work-api-redesign-decisions.md` §Droppable, §Affinity, §Deferred Instrumentation
- `conflux-work-api-redesign-deferred-plan.md` §Phases 5–8
- `conflux-work-gaps-for-http-rebuild.md` — http-rebuild gap analysis (G2–G9, Doc-1–Doc-6)

**V10 deferred items not in scope:**
- "Migratable-domain coroutine policy" (V10 §1862) — affinity policy for coroutines that migrate
  across domain boundaries. Requires a domain registry or migration primitive that does not exist
  yet. Deferred to a future plan once Phase 5 coroutine patterns are in use.
- "Aggregate placement rules for combinators" (V10 §1731) — where `when_all`/`race` callbacks land
  across domain boundaries. Phase 2 uses the already-resolved carrier layer; no placement question
  arises there. Deferred until a domain registry exists.

## Legacy Bridge Decision (G2)

The http-client rebuild surfaced the question: is there a bridge between the
legacy `conflux.work` module (`Flow<T>`, `WorkPool`, `run_on`) and the new
root module (`root::Task<T>`, `root::Operation<T>`, etc.)?

**Decision: no generic bridge.** `from_root_*` / `to_root_*` adapters were
deliberately removed (decisions doc §Migration Progress). A `Flow<T> →
root::Task<T>` shim is a smell that no production code should rely on long-
term.

**Practical resolution for consumers that face this today:**

- **G2c (short-term):** keep the transport coroutine on legacy `Task` / `Flow`
  internally; expose `root::Task<HttpResult>` only at the public boundary by
  wrapping with `make_task_source<T>` + `commit_*`. This is the recommended
  path for the http rebuild's phase 1–2.
- **G2b (Phase 11):** port `FileReader::*_async` and `TlsAsyncStream` to
  return `root::Operation<T>`. These are driver-category work (state-machine
  I/O primitives); `Operation<T>` is the correct root category. Tracked as
  Phase 11 in the priority table with owner TBD; not owned by the carrier
  continuation plan but must not be left untracked.
- **G2a (banned):** adding a one-way `Flow<T>::into_root_task()` bridge is
  explicitly rejected — it creates a permanent seam between two vocabularies.

This decision is final and not revisited in any continuation phase. Consumers
who see this gap should resolve via G2c while G2b is being planned.

---

## Completed (Phases 1–4)

| Phase | Deliverable | Status |
|---|---|---|
| 1 | Carrier decision (Model A), Chain<T>, from_task/posted/operation, map/then | done |
| 2 | when_all, when_all_fast_fail, race, Scope + admit | done |
| 3 | CancelReason::deadline, DeadlineScope | done |
| 4 | hop_to_posted/operation, verify_hop, HopCapabilityError, sentinel binding | done |

### Phase 2 Addendum: `when_all_fast_fail` Naming Clarification

`when_all_fast_fail` (carrier_model_a.cxx:181–188) currently behaves identically
to `when_all` because the carrier is purely eager — both arms are already
resolved before either combinator runs. The "fast fail" name promises
sibling-cancel-on-first-failure, which the implementation does not deliver.

**Decision:** keep the name but document the gap explicitly. When 5c lands and
async coroutines can produce in-flight `Chain`s (via `await_chain`), revisit
to add real fast-fail semantics: cancel the still-pending sibling on the first
arm's failure.

**Until then:** D1 must state plainly that `when_all_fast_fail` is currently a
synonym for `when_all` and that consumers writing async coroutines should NOT
depend on sibling-cancellation behaviour. Track as a known-issue in D1.

Add to Phase 6 carrier completions (lower priority): a small refactor that
exposes the cancel-sibling hook so `when_all_fast_fail` can wire it once the
async path exists. **Track as a concrete code TODO in `carrier_model_a.cxx`
at the `when_all_fast_fail` definition site (not just in this doc):**
`// TODO(phase-6): wire cancel-sibling hook once 5c async path lands;
currently identical to when_all` — so a code reader cannot miss the gap.

The existing source comment at carrier_model_a.cxx:181-188 reads "API
contract: in async context failure triggers best-effort sibling
cancellation before waiting for the loser to complete." That promises
behaviour the eager carrier does not deliver. Replace with the concrete
TODO above in the same Phase 6 PR; do not leave the aspirational text
in source.

### Phase 3 Addendum: Cancel + Deadline Precedence

`Scope::cancel` is **first-writer-wins** on `cancel_reason_`
(carrier_scope.cxx:84–86 — second call early-outs on the cancelled-flag
check). When a parent `Scope` cancel races with a `DeadlineScope` deadline
firing, the first one to claim the flag wins; the second is silently dropped
from the recorded reason. **No reason takes static precedence — whichever
thread CASes the flag first defines the recorded reason.**

This matches the actual carrier behaviour today. An earlier draft of this
plan asserted "`CancelReason::requested` always wins" — that was a
specification fiction; carrier_scope.cxx implements no such rule. Spec is
now amended to match code rather than the reverse, because (a) changing
the CAS to a multi-writer reason-priority resolution would require either
a second CAS loop or holding the cancel lock longer, and (b) consumers
already running against the carrier expect first-writer-wins semantics.

**Document in D2:** the recorded `cancel_reason_` is whichever cancel
source fires first. `requested` and `deadline` race like any other
cancel sources. Consumers that want to know "was this a deadline?"
can check the `DeadlineScope` itself (deadline scope tracks expiry
independently) rather than relying on the recorded reason.

Tests to add (Phase 3 backfill, low priority): two-thread test that races
`Scope::cancel(requested)` against `DeadlineScope` expiry; assert the recorded
`cancel_reason_` is whichever fired first AND that no `std::terminate` occurs
on the loser. **Do NOT assert `requested`-wins precedence — that contradicts
the implementation.**

## Design Principles For All Remaining Phases

- Root (root.cxx) is changeable when ergonomics or performance justify it.
  Changes must preserve the V10 semantic contracts (liveness, capability
  enforcement, cancellation model, allocation rules). Refactoring the surface
  or internal layout is fair game.
- Carrier layer (carrier_model_a.cxx, carrier_scope.cxx, carrier_deadline.cxx)
  remains unchanged unless a phase explicitly extends it additively. **Exception:**
  Phase 6 `Scope::admit` auto-bind is a documented semantic strengthening (not
  purely additive); it is the only carrier-behaviour change permitted by this
  plan and is gated on the audit step in Phase 6.
- Every new API surface works in both traditional (callback/chain) style and
  coroutine style. New coroutine-specific helpers are additive only.
- Performance: new paths must meet or beat equivalent traditional-style paths.
  Any phase that touches hot-path structs requires a benchmark comparison
  against the Phase 1 table before merge.
- No hidden runtime: the constraint from Phase 2 ("no hidden helper runtime")
  applies to all phases. Coroutines must not introduce background threads,
  thread-local state, or global executor registry.

---

## Root Layer Changes

These are standalone root.cxx improvements that unblock or improve multiple
downstream phases. They may land independently in any order.

### R1: `JoinContextError` Non-Final

`JoinContextError` is currently `final`. Removing `final` has zero performance
or semantic impact and immediately enables:

- `HopCapabilityError : public JoinContextError` (correct hierarchy — Phase 6)
- `JoinContextReason::hop_capability_mismatch` in the enum (Phase 6)
- Any future carrier or subsystem misuse error inheriting from the right base

The `JoinContextReason` enum aligns with V10 §1282–1286 base values and adds
`hop_capability_mismatch` as the single plan extension:

```cpp
// Before:
class JoinContextError final : public WorkError { using WorkError::WorkError; };

// After:
class JoinContextError : public WorkError {
    // Default-init: legacy throw sites that go via `using WorkError::WorkError`
    // never set reason_; reading reason() on those would be UB without this.
    JoinContextReason reason_ = JoinContextReason::unspecified;
public:
    // Preserve existing constructor set via inheritance — all existing throw
    // sites using string-only construction continue to compile unchanged.
    using WorkError::WorkError;
    explicit JoinContextError(std::string_view msg,
                              JoinContextReason reason);
    [[nodiscard]] JoinContextReason reason() const noexcept;
};

enum class JoinContextReason : std::uint8_t {
    unspecified,                       // default; existing throw sites not yet updated
    capability_mismatch,               // V10 §1282 — wrong capability passed to join
    thread_precondition,               // V10 §1283 — thread affinity precondition violated
    reentrant_pump,                    // V10 §1284 — reentrant pump detected
    hop_capability_mismatch,           // plan extension — set by HopCapabilityError
    ready_callback_already_installed,  // R2: try_set_on_ready single-consumer rule violated
};
```

Divergence note: V10 §1279 specifies `JoinContextError : std::logic_error`.
Current root.cxx has `JoinContextError : WorkError` (which inherits
`std::runtime_error`). This plan keeps `WorkError` as the base — it is the
established root hierarchy; changing `WorkError`'s base would be a larger
break. The divergence is deliberate and documented here.

Existing root `join(...)` throw sites update to use the appropriate reason
enum value. All current string-only catchers continue to work (`what()` string
preserved).

**Carrier-side source change called out:** Phase 6 reparents
`HopCapabilityError` to `JoinContextError`. `HopCapabilityError` is
currently `final` at carrier_model_a.cxx:46-49 — the `final` qualifier
must be removed in the same PR as Phase 6's reparent. Track as a
sub-task; not a separate phase.

Exit criteria:
- All existing root tests pass unchanged
- `HopCapabilityError : JoinContextError` compiles and is catchable as
  `JoinContextError`, `WorkError`, and `std::exception`
- `JoinContextError::reason()` returns the documented value at each throw site
- `what()` strings at all existing throw sites are unchanged

### R2: Control Block `on_ready` Callback

The control block notifies via `ready_cv_.notify_all()`. Adding a one-shot
ready callback enables coroutine suspension (Phase 5c), droppable handles
(Phase 8), and `std::execution` operation states (Phase 7).

**API shape — `try_set_on_ready` returning a status enum, NOT `set_on_ready`
returning void.** The void-returning shape with implicit "fire immediately if
already terminal" is unsafe for coroutine awaiters: a terminal commit racing
between `await_ready()` and `await_suspend()` causes the callback to fire
inline, the coroutine to resume + destroy the awaiter while `await_suspend`
is still on the stack. Reject that shape. Use:

```cpp
enum class ReadyRegistration : std::uint8_t {
    installed,         // callback stored; will fire on terminal commit
    already_ready,     // terminal already committed; caller may run rejected_fn
    already_installed, // a callback was already installed; this one is in rejected_fn
    empty,             // BasicControl had no underlying core (moved-from / default)
};

// Result struct: returns the rejected callable on any non-installed status,
// so move-only fn can be re-invoked or re-stored by the caller. Without this,
// a fn moved into try_set_on_ready cannot be recovered for the inline-fire
// path on already_ready, breaking move-only callable support entirely.
struct ReadyRegistrationResult {
    ReadyRegistration              status;
    MoveOnlyFunction<void()>       rejected_fn;  // empty iff status == installed
};

[[nodiscard]] virtual ReadyRegistrationResult
try_set_on_ready(MoveOnlyFunction<void()> fn) noexcept = 0;
```

The primitive does NOT invoke the callback inline. Awaiters use the `bool
await_suspend` form (see 5c). A convenience helper `set_on_ready_or_run` IS
required on `BasicControl` for non-coroutine callers that want "fire
immediately if ready, install otherwise" semantics — see protocol section
below. Implementing it ad-hoc at every call site is forbidden because the
precheck race must be handled correctly.

Single-consumer rule: at most one callback may be installed across the lifetime
of a control block. Second registration returns `already_installed`; in debug
builds also asserts.

**API placement:** `try_set_on_ready` goes on `ControlBlockBase` (non-templated)
and is forwarded by `BasicControl<Category>`. Callback is `void()` — closes over
the consumer handle, not the value.

```cpp
// Added to ControlBlockBase:
[[nodiscard]] virtual ReadyRegistrationResult
try_set_on_ready(MoveOnlyFunction<void()> fn) noexcept = 0;

// Added to BasicControl<Category>:
[[nodiscard]] ReadyRegistrationResult
try_set_on_ready(MoveOnlyFunction<void()> fn) noexcept {
    if (!core_) return {ReadyRegistration::empty, std::move(fn)};
    return core_->try_set_on_ready(std::move(fn));
}

// Added to ControlBlockModel<T, ...>:
enum class ReadyHookState : std::uint8_t {
    open,        // no callback installed; commit has not yet claimed
    armed,       // callback installed; commit must extract under hook_mtx_
    committing,  // commit is in progress; try_set_on_ready must NOT install
    terminal,    // outcome is published; try_set_on_ready returns already_ready
    disarmed,    // callback was explicitly cleared via R7 clear_on_ready;
                 // single-consumer rule still applies — no re-registration
};
std::atomic<ReadyHookState>  ready_hook_state_{ReadyHookState::open};
MoveOnlyFunction<void()>     on_ready_fn_{};           // protected by hook_mtx_
```

**State machine — eliminates lost-callback race.** Earlier draft used a
two-tier `on_ready_armed_` atomic + `terminal_claimed_` gate; that was racy
because (a) `terminal_claimed_` is set BEFORE outcome publish (root.cxx:741 vs
root.cxx:881–883), so an awaiter resuming on `already_ready` would block in
`root::join` waiting on `ready_cv_`, and (b) commit's `armed` check could
observe `open` and skip the hook mutex entirely while a concurrent
`try_set_on_ready` was just about to flip armed → callback lost. Both bugs
collapse if the gate IS the published-outcome boundary.

```cpp
// commit_* hot path (after outcome is fully constructed in storage but BEFORE
// it is published to readers):
auto prev = ReadyHookState::open;
// Try the lock-free fast path: open → committing.
if (ready_hook_state_.compare_exchange_strong(
        prev, ReadyHookState::committing,
        std::memory_order_acq_rel, std::memory_order_acquire)) {
    // No callback was ever installed. Publish outcome, mark terminal.
    publish_outcome_(/* terminal_state_ store-release */);
    ready_hook_state_.store(ReadyHookState::terminal,
                            std::memory_order_release);
} else if (prev == ReadyHookState::armed) {
    // Callback was installed and not cleared. Take the mutex, extract the
    // callback, publish outcome, mark terminal, then invoke under no lock.
    MoveOnlyFunction<void()> fn;
    {
        std::unique_lock lk{hook_mtx_};
        fn = std::move(on_ready_fn_);
        publish_outcome_();
        ready_hook_state_.store(ReadyHookState::terminal,
                                std::memory_order_release);
    }
    if (fn) fn();
} else if (prev == ReadyHookState::disarmed) {
    // Callback was installed then explicitly cleared via R7 clear_on_ready.
    // on_ready_fn_ is empty by construction; publish outcome and mark terminal.
    // Take hook_mtx_ to serialize with any racing try_set_on_ready precheck.
    {
        std::unique_lock lk{hook_mtx_};
        publish_outcome_();
        ready_hook_state_.store(ReadyHookState::terminal,
                                std::memory_order_release);
    }
} else {
    // prev == committing or terminal: impossible for a single-producer commit.
    // committing means another commit is in flight (would violate
    // terminal_claimed_ exclusion); terminal means commit already happened.
    std::abort();
}
ready_cv_.notify_all();  // existing path unchanged
```

The no-hook path is one CAS (acq_rel) plus the existing notify; no mutex.
Cost: one extra CAS vs Phase 4 baseline — measured against the < 2% regression
gate below.

**try_set_on_ready protocol (matched to the state machine).** Lock-first
ordering is **load-bearing**. An earlier draft of this plan had CAS
open→armed BEFORE locking `hook_mtx_`; that left a window where commit could
observe `armed`, take the lock first, and find an empty `on_ready_fn_` →
callback lost. Same race class R2 was meant to eliminate. The protocol below
holds `hook_mtx_` ACROSS the open→armed CAS and the fn store — commit's
armed-branch lock acquisition cannot interleave between them.

```cpp
ReadyRegistrationResult try_set_on_ready(MoveOnlyFunction<void()> fn) noexcept {
    std::unique_lock lk{hook_mtx_};                                 // (1) lock first

    auto s = ready_hook_state_.load(std::memory_order_acquire);
    if (s == ReadyHookState::terminal)
        return {ReadyRegistration::already_ready, std::move(fn)};   // give fn back
    if (s == ReadyHookState::armed || s == ReadyHookState::disarmed)
        return {ReadyRegistration::already_installed, std::move(fn)};
    if (s == ReadyHookState::committing) {
        lk.unlock();                                                // do NOT hold hook_mtx_ during spin
        spin_until_terminal_();                                     // commit holds hook_mtx_ in armed branch
        return {ReadyRegistration::already_ready, std::move(fn)};
    }
    // s == open
    auto expected = ReadyHookState::open;
    if (!ready_hook_state_.compare_exchange_strong(
            expected, ReadyHookState::armed,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        if (expected == ReadyHookState::committing) {
            lk.unlock(); spin_until_terminal_();
            return {ReadyRegistration::already_ready, std::move(fn)};
        }
        if (expected == ReadyHookState::terminal)
            return {ReadyRegistration::already_ready, std::move(fn)};
        return {ReadyRegistration::already_installed, std::move(fn)};
    }
    on_ready_fn_ = std::move(fn);                                   // (2) store under same lock
    return {ReadyRegistration::installed, {}};
}
```

**try_set_on_ready return semantics — fn returned on non-installed.** The
result struct's `rejected_fn` carries the callable back on every status
except `installed`. This is mandatory for move-only callables; an earlier
draft that consumed fn unconditionally could not implement
`set_on_ready_or_run` for the common case (closures capturing
`coroutine_handle<>` + scheduler reference are themselves move-only).
`5c` awaiters discard `rejected_fn` because they re-enter `await_resume`
which calls `root::join` directly. Other consumers (instrumentation,
ad-hoc) inspect the rejected lambda and decide their own policy.

**Gate semantic — load-bearing.** `already_ready` MUST mean "outcome is
visible to a subsequent `root::join` without blocking on `ready_cv_`." The
state machine satisfies this because `terminal` is published only after the
outcome store. Awaiters relying on this (5c) can call `join` from
`await_resume` without risk of blocking. Earlier `terminal_claimed_` gate
violated this invariant.

**Spin-until-terminal helper.** The `committing` branch transitions to
`terminal` within microseconds (commit's armed-branch holds hook_mtx_ +
publishes outcome + stores terminal). Implementation MUST NOT hold
`hook_mtx_` while waiting for `terminal` — that would deadlock against
commit's armed-branch which itself needs `hook_mtx_` to publish:

```cpp
void spin_until_terminal_() noexcept {
    constexpr int kBoundedSpins = 64;   // ~64 × pause = sub-microsecond
    for (int i = 0; i < kBoundedSpins; ++i) {
        if (ready_hook_state_.load(std::memory_order_acquire)
            == ReadyHookState::terminal) return;
        cpu_relax();                     // arch wrapper — x86 pause, arm yield, etc.
    }
    // Bounded spin exceeded → yield WITHOUT holding hook_mtx_; commit needs
    // that lock to publish in the armed branch.
    constexpr auto kAbortAfter = std::chrono::milliseconds{10};
    auto deadline = std::chrono::steady_clock::now() + kAbortAfter;
    while (ready_hook_state_.load(std::memory_order_acquire)
           != ReadyHookState::terminal) {
        if (std::chrono::steady_clock::now() > deadline) {
            // Commit is genuinely stuck (deadlocked or panicked publisher).
            // Surface the bug rather than peg a CPU forever.
            std::abort();
        }
        std::this_thread::yield();
    }
#ifndef NDEBUG
    // Optional: count spin-fallback events for diagnostics.
#endif
}
```

Spin must continue UNTIL `terminal` observed — `committing` does NOT
guarantee outcome is published yet (publish happens between CAS→committing
and store→terminal). Returning `already_ready` after only an N-iteration
exit would violate the gate semantic. **Spin count lowered from 1024 to 64**
(reviewer note: 1024 × ~3ns = ~3µs CPU burn under contention is too
aggressive; 64 keeps the bounded-spin window sub-microsecond and lets
yield take over).

**Lock order — universal.** Full chain: `hook_mtx_ < abandon_mtx_ <
outcome_mtx_`. The armed-branch commit holds `hook_mtx_` then
`outcome_mtx_` inside `publish_outcome_()`. The existing
`run_abandon_path_if_present()` (root.cxx:762) takes `{abandon_mtx_,
outcome_mtx_}`. Document the full triple order so future R2-aware code
does not deadlock by acquiring in reverse.

**Already-ready precheck.** To support the `bench/already_ready_no_lock`
exit criterion, `try_set_on_ready` MAY precheck the state atomic before
locking — `terminal` is final, so observing it without the lock is safe:

```cpp
ReadyRegistrationResult try_set_on_ready(MoveOnlyFunction<void()> fn) noexcept {
    // Fast-path: terminal is final; no lock needed to confirm.
    if (ready_hook_state_.load(std::memory_order_acquire)
        == ReadyHookState::terminal)
        return {ReadyRegistration::already_ready, std::move(fn)};

    std::unique_lock lk{hook_mtx_};
    // ... rest of protocol as above ...
}
```

The precheck races with concurrent commit but is sound: if precheck reads
not-terminal then commit reaches terminal, the locked path observes it
inside the mutex; if precheck reads terminal, the gate semantic guarantees
outcome is already published.

**`set_on_ready_or_run` is mandatory, not optional.** The fire-or-store
helper relies on the rejected-callable return so the move-only `fn`
materialised into `try_set_on_ready` is given back if the install fails:

```cpp
template<class F>
    requires std::invocable<F> && std::is_nothrow_invocable_v<F>
void set_on_ready_or_run(F &&fn) noexcept {
    auto materialised = MoveOnlyFunction<void()>{std::forward<F>(fn)};
    auto result = try_set_on_ready(std::move(materialised));
    switch (result.status) {
        case ReadyRegistration::installed:
            return;                              // will fire on commit thread
        case ReadyRegistration::already_ready:
            // Outcome is published; fn was returned in rejected_fn.
            // Fire inline on caller's thread. Caller-stack semantics
            // intentionally differ from the install branch (which fires
            // on commit thread) — document in D1.
            if (result.rejected_fn) result.rejected_fn();
            return;
        case ReadyRegistration::already_installed:
        case ReadyRegistration::empty:
            // Single-consumer contract violated OR moved-from control;
            // drop the callback. `result.rejected_fn` holds the moved-back
            // callable but `set_on_ready_or_run` cannot resurrect it: the
            // contract is "fire-or-store", not "fire-or-fire-anyway", and
            // executing a callback that another consumer's protocol expects
            // to own would be a worse silent bug. Debug build asserts.
            // Move-only-callable callers that need recovery semantics MUST
            // call try_set_on_ready directly and inspect rejected_fn.
            return;
    }
}
```

Document explicitly: this is the supported pattern for non-coroutine
callers that want "fire immediately if ready, install otherwise"
semantics. Implementing it ad-hoc at every call site is forbidden because
the precheck race must be handled correctly. **Ship this in the same R2
PR**, not as a follow-up.

**`fn` must be `is_nothrow_invocable_v<F>`** — concept-enforced. Throwing
callables are rejected at compile time. If a callback violates noexcept
dynamically (escaping exception from a function declared noexcept-but-not),
both fire sites — caller stack on `already_ready` and commit thread on
`installed` — abort via `std::terminate` per the noexcept contract. There
is no propagation path; the helper provides uniform termination semantics.
D1 documents both fire sites and explicitly the no-propagation guarantee.

**Abandon-clears-on_ready — R2 contract (root-level, NOT just D3).**
Because R2 introduces `on_ready_fn_`, R2 itself must define what happens
to it on producer-side abandonment. The contract:

```text
Any root-side abandon or destruction path that releases a non-terminal
control block MUST clear on_ready_fn_ under hook_mtx_ before releasing
its strong reference. Implementations may instead transition the control
block to terminal cancelled (firing the callback as part of normal R2
armed-branch commit) — both satisfy the contract.
```

Phase 8 DroppableSlot drain depends on this; D3 documents the consumer
side. R2 owns the producer side. **Audit all root-side strong-ref-drop
paths for compliance:** `~BasicSource` (root.cxx:1306-1310 — calls
`commit_cancelled(abandoned, true)` → terminal commit → satisfies
contract via R2 armed branch), `request_cancel`-without-commit (root.cxx:797
— does NOT terminal-commit → MUST clear on_ready_fn_ explicitly if R2
admitted a callback). Any new abandon path lands with this audit.

**Threat-model audit conclusion:** the previously-cited "producer abandon
WITHOUT commit_*" path is largely covered by `~BasicSource`'s explicit
`commit_cancelled(abandoned)`. The remaining real path is consumer-side
`abandon_to(...)` (root.cxx:938-952) which sets `abandoned_=true` but
does NOT clear `on_ready_fn_` today. R2's contract requires this path be
updated to clear under `hook_mtx_` if a callback is installed.

**`clear_on_ready()` primitive — added to ControlBlockBase.** Required for
defensive callers (e.g., `~TaskHandleAwaiter` reclaiming an installed
callback before abandon). Returns a status enum so callers can distinguish
whether they actually reclaimed an in-flight callback or whether commit
has already raced past:

```cpp
enum class ClearOnReadyStatus : std::uint8_t {
    cleared,         // armed → disarmed; on_ready_fn_ dropped; safe to abandon
    in_flight,       // commit is already in committing branch; lambda WILL fire
                     // — caller cannot prevent it; must NOT destroy referent yet
    already_terminal,// terminal reached; no callback pending; safe to abandon
    not_armed,       // open or disarmed; no-op
};

// ControlBlockBase:
[[nodiscard]] virtual ClearOnReadyStatus clear_on_ready() noexcept = 0;

// ControlBlockModel:
[[nodiscard]] ClearOnReadyStatus clear_on_ready() noexcept override {
    std::unique_lock lk{hook_mtx_};
    auto s = ready_hook_state_.load(std::memory_order_acquire);
    if (s == ReadyHookState::committing)  return ClearOnReadyStatus::in_flight;
    if (s == ReadyHookState::terminal)    return ClearOnReadyStatus::already_terminal;
    if (s != ReadyHookState::armed)       return ClearOnReadyStatus::not_armed;
    on_ready_fn_ = {};
    // Transition armed → disarmed (NOT open) so single-consumer rule
    // remains: a re-registration after explicit clear returns
    // already_installed. Open is reserved for "never installed in this
    // lifetime."
    ready_hook_state_.store(ReadyHookState::disarmed,
                            std::memory_order_release);
    return ClearOnReadyStatus::cleared;
}
```

**Ownership discipline — load-bearing.** `clear_on_ready` must ONLY be
called by the consumer that actually installed the callback. If
`try_set_on_ready` returned `already_installed`, the callback belongs to
SOMEONE ELSE; calling `clear_on_ready` would steal it. Awaiters track an
internal `callback_installed_` flag (set true only on successful
`installed` status) and check it before calling `clear_on_ready`.

**`in_flight` status — soundness limit.** If `clear_on_ready` returns
`in_flight`, the lambda is mid-execution on the commit thread and will
invoke its captured handle. The caller (e.g., `~TaskHandleAwaiter`)
**cannot prevent this synchronously**. The defensive abandon downgrades
to "best-effort": if the awaiter is destroyed during the in_flight
window, the coroutine handle dispatch races with frame teardown → UB.
This is documented as the soundness limit of R2 + R7; eliminating it
would require a `clear_or_wait_on_ready` primitive that blocks the
caller until the lambda completes — deferred to a follow-up if a real
consumer needs it. Defensive awaiter destructors MUST treat `in_flight`
as a checked-build diagnostic emit + best-effort abandon.

**Existing `abandoned_` interaction — double-abandon hazard.** `install_abandon_sink`
(root.cxx:944-947) calls `std::terminate()` if `abandoned_` is already
set. R2's defensive `~TaskHandleAwaiter` calls `abandon_to(handle, ...)`
unconditionally if the handle is unconsumed. If a producer-side abandon
already fired (e.g., another consumer abandon, a `~BasicResult`), the
defensive abandon attempts a second install → terminate.

**Required — `try_abandon_to` is a hard prerequisite for Phase 5c.** Add a
new root primitive that returns a status enum instead of terminating:

```cpp
enum class AbandonStatus : std::uint8_t {
    installed,         // sink installed; ownership transferred
    already_abandoned, // a producer-side abandon already fired; handle
                       // still considered consumed and safe to drop
    empty,             // moved-from handle
};

template<class Sink>
[[nodiscard]] AbandonStatus
try_abandon_to(BasicJoinHandle<...> handle, Sink &&sink) noexcept;
```

All defensive awaiter destructors (Phase 5c, Phase 8) MUST use
`try_abandon_to`, not `abandon_to`. Treat `already_abandoned` as success
for ownership-transfer purposes (handle is consumed). `try_abandon_to`
ships in the same R2 PR — it is not a follow-up. Without it, the
defensive abandon is itself a terminate hazard on a path it claims to
protect.

**Existing `terminal_claimed_` interaction — disposition audit.** R2 adds
`ready_hook_state_` but does not remove `terminal_claimed_`. The latter
is still used by:

| Site | Disposition |
|---|---|
| `commit_success` (root.cxx:874-887) | Full R2 protocol body shown above. Sequence: claim_terminal → CAS ready_hook_state_ → publish_outcome_ (under outcome_mtx_) → store terminal_state_ → store ready_hook_state_=terminal → release hook_mtx_ (armed branch) → invoke fn → call run_abandon_path_if_present LAST (it takes abandon_mtx_+outcome_mtx_). The fn() runs BEFORE run_abandon_path because abandon path may dispatch another callback that observes the on_ready effect. |
| `commit_failure` (root.cxx:889-902) | Identical sequence — same CAS + publish + store + fn + run_abandon_path_if_present at end. |
| `commit_cancelled` (root.cxx:904-922) | Identical sequence. |
| `install_cancel_hook` (root.cxx:841-872) | Continues to gate on `terminal_claimed_`; cancel-hook is independent of R2's ready callback. No R2 interaction. |
| `claim_requested_hook_if_present` (root.cxx:721) | Independent — cancel-hook plumbing. No R2 interaction. |
| `request_cancel` (root.cxx:797) | Independent — does not terminal-commit. No R2 interaction unless cancellation transitions to terminal (then armed-branch fires R2 callback). |
| `ready()` derivation | Reads `terminal_state_` (set under outcome_mtx_); orthogonal to R2's hot-path state. |
| `~BasicSource` (root.cxx:1306-1310) | Calls commit_cancelled(abandoned, true) — terminal commit → fires R2 armed branch correctly. No additional clear_on_ready needed. |
| `install_abandon_sink` (root.cxx:938-952) | Sets abandoned_=true, stores sink — does NOT clear on_ready_fn_. Per R2 abandon-clears-on_ready contract this MUST be updated to clear under hook_mtx_ if a callback is installed (otherwise the lambda fires later on a sink-only abandoned control block, which depending on lambda contents can resurrect a dead consumer). |

**Net cost:** every commit pays one acq_rel CAS on `ready_hook_state_`
(R2) plus the existing `terminal_claimed_` flag write. The split perf
gates (real-work <2%, minimal <5%) account for this overhead. If contended
benchmarks show the dual-write is too costly, a follow-up may collapse
`terminal_claimed_` into a `ready_hook_state_` derived view — gated on
post-implementation measurement.

**MoveOnlyFunction SBO measurement — gating step.** Before R2 lands, measure
the actual SBO size of the in-tree `MoveOnlyFunction<void()>` at root.cxx:511
(the `callable_erasure` site). Record the number. `InlineBytes` constant in
root.cxx is currently 32; the guard-band below is computed from that, NOT from
a fictional 80-byte budget.

**Control block layout audit required before landing.** Measure
`sizeof(ControlBlockModel<int, false>)` before and after; record in commit.

**Size guard-band:** `sizeof(ControlBlockModel<int, false>)` after R2 must not
exceed pre-R2 size + `kReadyHookGuardBand` where:

```cpp
constexpr std::size_t kReadyHookGuardBand =
    sizeof(MoveOnlyFunction<void()>)         // on_ready_fn_ — full struct
  + sizeof(std::atomic<ReadyHookState>)      // ready_hook_state_
  + 8;                                       // padding/headroom
```

`sizeof(MoveOnlyFunction<void()>)` resolves at compile time and is NOT the
same as `InlineBytes`. Today `InlineBytes = 32` (root.cxx:507); the full
struct is larger (32-byte buffer + vtable pointer + bool + alignment padding,
typically ~80 bytes — measure as part of the audit, do not assume). The band
is therefore on the order of ~96 bytes today, NOT 48. Earlier draft asserted
"~48" — that conflated `InlineBytes` with `sizeof(MoveOnlyFunction)` and
would have made the audit gate trivially passable. Use the symbolic constant;
do not write a numeric budget in the test. Exceeding the band →
`on_ready_fn_` becomes `MoveOnlyFunction<void()>*` pointing to a
lazily-allocated `HookState` sidecar shared with the cancel hook (one
indirection, both hooks share one cache line). Sidecar fallback impl MUST
land in the same PR if the audit exceeds the band — do not defer. Document
choice in commit.

**Ordering contract:** Callback fires after `terminal_state_` is published,
before `ready_cv_.notify_all()`, and before `run_abandon_path_if_present()`.
Runs on the producer's commit thread. Inline coroutine resumption thus runs on
the producer thread. Latency-sensitive producers must post the resume from
inside the callback rather than resume inline. **This is also true for
already-ready returns:** when `try_set_on_ready` returns `already_ready`, the
caller — not R2 — chooses whether to invoke `fn` synchronously, post it, or
drop it. `5c await_suspend` chooses post-or-drop; see 5c for detail.

**Post-back helper — boilerplate avoidance.** Every consumer that wants to
resume on a specific context (lane, scheduler, owner) writes the same pattern:

```cpp
handle.control().try_set_on_ready(
    [h, sched]() mutable noexcept { sched.post([h]() mutable noexcept { h.resume(); }); });
```

Provide a single helper:

```cpp
// In a new header, e.g. conflux.work.carrier.coro:
template<class Sched>
concept resume_scheduler = requires(Sched &s, MoveOnlyFunction<void()> fn) {
    { s.post(std::move(fn)) } noexcept -> std::same_as<void>;
};

template<resume_scheduler Sched>
[[nodiscard]] MoveOnlyFunction<void()> resume_on(std::coroutine_handle<> h,
                                                 Sched &sched) noexcept;
```

Returns a noexcept callable that posts `h.resume()` through `sched.post(...)`.

**Sched contract — locked down:**
- `Sched::post(MoveOnlyFunction<void()>)` MUST be `noexcept` (the concept
  enforces this). A scheduler that can fail at submit time is not a valid
  `resume_scheduler` — failure modes belong above this layer.
- `Sched&` is captured **by reference** in the returned callable. Lifetime
  contract: the scheduler MUST outlive every coroutine that resumes through
  it. Posted/Operation owners satisfy this (the awaiter holds the handle which
  pins the owner). Lane references satisfy this (lanes outlive their work).
  Do NOT use this helper with a stack-local scheduler.
- **No compile-time enforcement of the outlive-coroutine lifetime contract**
  is currently provided. A common foot-gun is wrapping a stack-local lambda
  in an ad-hoc scheduler. Recommended pattern: pass schedulers through a
  shared owning wrapper (`std::shared_ptr<SchedT>` or a long-lived registry
  reference). D1 must include this pattern as the canonical example. A
  follow-up may add `static_assert` rejection of rvalue references / temporary
  lifetimes once a portable detection mechanism exists.
- The returned callable is `noexcept`. If the post itself fails inside Sched
  (kernel error, queue at capacity), Sched MUST handle it internally —
  typically by terminating, not by silently dropping. Dropped resumes leak
  coroutine frames and violate root liveness contracts of any handle the
  coroutine owns.
- For a Sched that genuinely cannot guarantee post success (e.g., bounded
  queue), wrap it in an adapter that retries / blocks / terminates per its
  policy before passing to `resume_on`.

This is the recommended primitive used by `PostedHandleAwaiter` (when shipped)
and any user code that needs context-correct resumption.

Exit criteria:
- `try_set_on_ready` compiles on `ControlBlockBase`; `BasicControl` forwarder
  present; `[[nodiscard]]` enforced
- `MoveOnlyFunction<void()>` SBO size measured at root.cxx:511 and recorded
  in commit; final guard-band computed from that number
- `sizeof(ControlBlockModel<int, false>)` measured before and after; recorded
  in commit; must not exceed the computed band (or `HookState*` sidecar
  fallback chosen and documented)
- `ready_hook_state_` CAS implemented; benchmark gate split by baseline cost:
  `root/task_join_success_real_work` (commit thread does ≥1µs of work
  surrounding commit) regression < 2%; `root/task_join_success_minimal`
  (commit + immediate join, no surrounding work) regression < 5%. Splitting
  acknowledges that one extra acq_rel CAS (~10–20 cycles uncontended) is a
  larger relative fraction of a sub-100ns baseline than of a 1µs one. A
  flat <2% gate against the minimal benchmark would be unrealistically
  tight given the redesigned protocol.
- Contended micro-bench: simultaneous `try_set_on_ready` + `commit_success`
  on N=8 threads × 100k iterations; record p50/p99 latency, assert no
  regression vs Phase 4 baseline beyond the documented band
- Already-ready fast-path bench: `try_set_on_ready` on a pre-committed
  control block returns `already_ready` without taking `hook_mtx_`
- Stress test: 1000 iterations of two threads — one calling `try_set_on_ready`,
  one calling `commit_success` — asserting (a) callback fires exactly once when
  status was `installed`, (b) status `already_ready` paths satisfy the
  no-blocking-join invariant (subsequent `root::join` returns immediately
  without waiting on `ready_cv_`), (c) no lost callbacks across the race
  window — this is the bug R2 redesign fixes
- Stress test: second `try_set_on_ready` while first still pending returns
  `already_installed` and does not overwrite first
- Test: `try_set_on_ready` after terminal commit returns `already_ready`
  without invoking `fn` AND `root::join` immediately afterward returns the
  outcome without blocking (proves the gate-on-published-outcome invariant)
- TSAN run on the contended micro-bench: zero data races reported
- No regression on `root/posted_join_success`, `root/cancel_hook_*` benchmarks

### R3: `Outcome<T>` Ergonomics

`Outcome<T>` currently requires manual `.is_success()` + `.success().value`
branches. Adding shortcut accessors removes boilerplate at every call site.

**`value()` — throwing extractor:**

```cpp
[[nodiscard]] T &      value() &;
[[nodiscard]] T const& value() const &;
[[nodiscard]] T        value() &&;
```

On failure: rethrows the captured cause directly via
`std::rethrow_exception(failure().error)` — same semantics as
`std::future::get()`. On cancellation: throws `root::CancelledError`. This
avoids wrapping the original user exception in `FailureError`, which would
require the caller to unwrap with `.cause()` to reach the actual exception.

For `void` specialization — single overload suffices (no `T` to move out, so
ref-qualifiers add no value):

```cpp
void value() const;
```

**`match()` — three-branch visitor:**

The existing single-functor `Outcome<T>::visit` is not changed. The three-branch
form is named `match` to avoid overload collision and to distinguish it from
the single-functor inspection. Ref-qualified overloads to support move-only
`T` correctly:

```cpp
// Rvalue: success branch receives T by rvalue, supports move-only T.
template<class OnSuccess, class OnFailure, class OnCancelled>
    requires std::invocable<OnSuccess, T&&>
          && std::invocable<OnFailure, Failure const&>
          && std::invocable<OnCancelled, Cancelled const&>
          && std::same_as<
                std::invoke_result_t<OnSuccess, T&&>,
                std::invoke_result_t<OnFailure, Failure const&>>
          && std::same_as<
                std::invoke_result_t<OnSuccess, T&&>,
                std::invoke_result_t<OnCancelled, Cancelled const&>>
auto match(OnSuccess&&, OnFailure&&, OnCancelled&&) &&
    -> std::invoke_result_t<OnSuccess, T&&>;

// Lvalue: success branch receives T const&, no move out.
template<class OnSuccess, class OnFailure, class OnCancelled>
    requires std::invocable<OnSuccess, T const&>
          && std::invocable<OnFailure, Failure const&>
          && std::invocable<OnCancelled, Cancelled const&>
          && std::same_as<
                std::invoke_result_t<OnSuccess, T const&>,
                std::invoke_result_t<OnFailure, Failure const&>>
          && std::same_as<
                std::invoke_result_t<OnSuccess, T const&>,
                std::invoke_result_t<OnCancelled, Cancelled const&>>
auto match(OnSuccess&&, OnFailure&&, OnCancelled&&) const &
    -> std::invoke_result_t<OnSuccess, T const&>;
```

The rvalue overload calls `std::invoke(on_success, std::move(success().value))`
internally — required for move-only `T` to compile.

**`Outcome<void>::match()` specialisation — ship in same PR, not later:**

```cpp
template<class OnSuccess, class OnFailure, class OnCancelled>
    requires std::invocable<OnSuccess>
          && std::invocable<OnFailure, Failure const&>
          && std::invocable<OnCancelled, Cancelled const&>
          && std::same_as<
                std::invoke_result_t<OnSuccess>,
                std::invoke_result_t<OnFailure, Failure const&>>
          && std::same_as<
                std::invoke_result_t<OnSuccess>,
                std::invoke_result_t<OnCancelled, Cancelled const&>>
auto match(OnSuccess&&, OnFailure&&, OnCancelled&&) const
    -> std::invoke_result_t<OnSuccess>;
```

`OnSuccess` takes no arg in the void specialisation; otherwise identical.

Explicit `std::same_as` constraints over deduced return type: mismatched branch
return types produce a clean `requires` failure at the call site, not a cascade
through `decltype(auto)` or `std::common_type`.

**Asymmetry note:** existing `visit(F)` passes `Success<T>&` (the wrapper) to
`F`. `match(on_s, ...)` passes `T` (unwrapped value) to `on_s`. This is
intentional: `visit` is for structural inspection of the outcome variant;
`match` is for extracting and transforming the unwrapped result. The asymmetry
is deliberate and documented here.

V10 §1890–1891 rejected a three-branch `visit` at root. This plan adds it as
`match` (distinct name, no overload conflict) and justifies the divergence:
root is now changeable, the deduction is explicit (no implicit narrowing), and
the ergonomic case is clear. The single-functor `visit` is unaffected.

Traditional code continues to compile unchanged (`release_outcome()` + manual
branch). New code uses `value()` for success-expected paths; `match()` for
exhaustive handling.

Exit criteria:
- `value()` rethrows the stored cause directly on failure; throws
  `CancelledError` on cancellation; returns `T` on success
- `value() const` for `Outcome<void>` compiles and throws correctly
- `match()` compiles for identical-return-type branches; `requires` clause
  fails for mismatched branches with a clear diagnostic
- `match() &&` compiles for move-only `T` (test with `std::unique_ptr<int>`);
  success branch sees `T&&`
- `Outcome<void>::match()` compiles and dispatches to a no-arg success branch
- `sizeof(Outcome<T>)` unchanged
- All existing `visit(fn)` call sites compile unchanged

### R4: `joinable` Free Function

Thin wrapper over `can_join`. Purely ergonomic.

```cpp
template<root::progress_capability Cap, root::work_value T>
[[nodiscard]] bool joinable(Cap const &cap,
                            root::PostedJoinHandle<T> const &h) noexcept;

template<root::progress_capability Cap, root::work_value T>
[[nodiscard]] bool joinable(Cap const &cap,
                            root::OperationJoinHandle<T> const &h) noexcept;
```

Exit criteria:
- `joinable(cap, h)` returns the same value as `can_join(cap, h)` for all
  combinations of matching/mismatching capability and live/dead handle
- No regression on existing carrier tests

### R6: `BasicJoinHandle` Liveness Probe

`BasicJoinHandle<Cat, T>` (root.cxx:1530–1594) holds a private `live_` flag
but exposes only `control()`. Defensive destructors (e.g.,
`~TaskHandleAwaiter` in 5c) need to test "is this handle still live?"
without consuming it. Without this, the awaiter dtor cannot know whether
to skip the defensive abandon or perform it.

```cpp
// Added to BasicJoinHandle<Cat, T>:
[[nodiscard]] explicit operator bool() const noexcept { return live_; }
```

Trivial change; no behaviour shift; required by 5c's defensive abandon
and any future caller that wants moved-from detection.

Exit criteria:
- `bool(handle)` returns true after `make_*_source(...).second` (first
  observation) and false after move-from
- No regression on existing root tests

### R7: Defensive `clear_on_ready` Primitive

See R2 §"clear_on_ready primitive". Specified there; cross-referenced from
R7 so the priority table can sequence it independently of R2's main
deliverable. Lands in same R2 PR.

### R8: `JoinContextReason::ready_callback_already_installed` Gating

The new enum value is referenced from 5c's `TaskHandleAwaiter::await_resume`.
**R1 may ship the enum value before 5c lands** — there are no other setters
in the meantime. Document this is a forward-declared value; first real
setter is in 5c. Avoid landing R1 + 5c in separate releases without
documenting the dead-enum-window in CHANGELOG.

### R5: Stop-State — Already Done

Phase 10a (lazy stop-state allocation) is already implemented.
`ControlBlockModel` uses `std::conditional_t<EnableCancellation,
std::stop_source, std::monostate>` — the `std::stop_source` is never
instantiated for `enable_cancellation = false` admissions. Close this item.

---

## Phase 5: Coroutine Carrier

### Goal

Make `Chain<T>` directly awaitable inside C++ coroutines, and provide a
coroutine return type that produces `Chain<T>`. This completes the
"traditional + coroutine" dual-use model for the synchronous carrier layer.

A second sub-phase adds async suspension for root join handles so coroutines
can suspend at real async boundaries.

### 5a: Synchronous co_await For Chain<T>

`Chain<T>` already has a resolved outcome. `co_await Chain<T>` extracts the
value synchronously (never suspends) and rethrows on failure or cancellation.

```cpp
// carrier_model_a.cxx — new internal awaiter
template<root::work_value T>
struct ChainAwaiter {
    Chain<T> chain_;

    [[nodiscard]] bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}

    decltype(auto) await_resume() {
        auto out = std::move(chain_).release_outcome();
        if (out.is_success()) {
            if constexpr (!std::same_as<T, void>)
                return std::move(out).success().value;
            else
                return;
        }
        if (out.is_failure())
            std::rethrow_exception(std::move(out).failure().error);
        throw root::CancelledError{out.cancelled().reason};
    }
};

// Added to Chain<T>:
[[nodiscard]] ChainAwaiter<T> operator co_await() && noexcept {
    return ChainAwaiter<T>{std::move(*this)};
}
```

Traditional and coroutine usage side by side:

```cpp
// Traditional (unchanged):
auto out = std::move(chain).release_outcome();
if (out.is_success()) use(out.success().value);

// Coroutine (new):
int value = co_await std::move(chain);  // rethrows on failure/cancellation
```

### 5b: EagerChain<T> — Coroutine Return Type

A coroutine returning `EagerChain<T>` runs eagerly and synchronously, can
`co_await Chain<T>` values inside its body, and produces a `Chain<T>` on
completion. Output type is still `Chain<T>` — existing combinators are
unchanged.

**Sync-only contract — load-bearing.** `EagerChain` is for coroutines that
await *only* already-resolved awaitables (`Chain<T>`, sub-`EagerChain<U>`).
Awaiting a real async awaiter (`TaskHandleAwaiter` from 5c, `DroppableSlot`
from Phase 8) inside an `EagerChain` body is a user error: the coroutine
would suspend, control would return to the `EagerChain` constructor with
`slot_` empty, AND the suspended frame would own a live root join handle by
value. Destroying that frame would destroy the live handle → root.cxx
liveness terminate. Runtime empty-slot detection happens too late.

**Statically prevent via `await_transform`.** Make EagerChain compile-error
on async awaitables rather than detecting at runtime:

```cpp
struct EagerChainPromise<T> {
    // ... slot_, get_return_object, etc ...

    // Whitelist: synchronous awaitables only.
    template<root::work_value U>
    auto await_transform(Chain<U> &&c) noexcept {
        return std::move(c).operator co_await();
    }
    template<root::work_value U>
    auto await_transform(EagerChain<U> &&e) noexcept {
        // Routes through .chain() so empty-slot protection runs.
        return std::move(e).chain().operator co_await();
    }

    // Hard-block any other awaitable. Compile error names the type at the
    // co_await site — much clearer than a runtime Failure-Chain.
    template<class Awaitable>
    auto await_transform(Awaitable &&) = delete;
};
```

`co_await TaskJoinHandle<T>` inside an `EagerChain` body now fails to
compile with the deleted-overload diagnostic. No async frame can suspend.

**Defense-in-depth: empty-slot runtime check stays.** Compiler/library edge
cases or future awaitable additions could still produce an empty slot. Keep
the `chain()` empty-slot check (returns Failure-Chain) and `~EagerChain`
benign destroy as the second line of defense. With `await_transform` in
place, this path is unreachable for in-tree awaitables.

**Defense-in-depth: `~TaskHandleAwaiter` abandons handle.** If a future
addition (or unrelated coroutine type) destroys a live `TaskHandleAwaiter`
without consuming it, `~TaskHandleAwaiter` MUST call
`root::abandon_to(noop_sink, std::move(handle_))` rather than letting
`~TaskJoinHandle` terminate. Specify in 5c.

For real async coroutines, **use `Task<T>` (root) or wait for an `AsyncChain<T>`
follow-up** (not in scope this plan). Document this in D1.

**Naming:** `EagerChain` communicates the contract (eager, synchronous
execution; produces a Chain). `CoroChain` was rejected — it says "coroutine"
but the output is a plain `Chain<T>` and the execution is synchronous.

**Promise internals:** The promise stores `std::optional<root::Outcome<T>>`
directly — no `root::TaskSource<T>`. `TaskSource<T>` requires control block
allocation (~80 ns per benchmark) and its destructor commits cancellation if
not explicitly closed; neither is appropriate for a synchronous coroutine
frame. The `slot_` lives inside the coroutine frame.

**Frame lifetime:** `final_suspend` returns `std::suspend_always` so the frame
is NOT destroyed at `co_return`. `EagerChain<T>` owns a
`std::coroutine_handle<EagerChainPromise<T>>` and destroys the frame in its
own destructor after `chain()` extracts `slot_`. This is the standard
ownership pattern. The coroutine frame itself heap-allocates (unless HALO
eliminates it — not guaranteed); the `std::optional<Outcome<T>>` inside the
frame adds no *additional* allocation beyond the frame itself.

```cpp
// New module: conflux.work.carrier.coro
template<root::work_value T>
class EagerChain;

// Promise type (internal):
template<root::work_value T>
struct EagerChainPromise {
    std::optional<root::Outcome<T>> slot_{};

    EagerChain<T> get_return_object() noexcept;
    std::suspend_never  initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }  // frame stays live until chain() extracts

    void unhandled_exception() {
        slot_ = root::Outcome<T>{root::Failure{std::current_exception()}};
    }

    // For non-void T:
    void return_value(T v) {
        slot_ = root::Outcome<T>{root::Success<T>{std::move(v)}};
    }
    // For void T:
    void return_void() {
        slot_ = root::Outcome<T>{root::Success<T>{}};
    }

    // Sync-only enforcement (see "Statically prevent via await_transform"
    // section). Whitelist Chain<U> and EagerChain<U>; delete everything else.
    template<root::work_value U>
    auto await_transform(Chain<U> &&c) noexcept {
        return std::move(c).operator co_await();
    }
    template<root::work_value U>
    auto await_transform(EagerChain<U> &&e) noexcept {
        return std::move(e).chain().operator co_await();
    }
    template<class Awaitable>
    auto await_transform(Awaitable &&) = delete;
};

template<root::work_value T>
class EagerChain {
    std::coroutine_handle<EagerChainPromise<T>> handle_;
public:
    using promise_type = EagerChainPromise<T>;

    explicit EagerChain(std::coroutine_handle<EagerChainPromise<T>> h) noexcept
        : handle_{h} {}
    ~EagerChain() noexcept { if (handle_) handle_.destroy(); }

    // ~EagerChain destroys the suspended frame WITHOUT calling std::terminate.
    // This intentionally diverges from root's hard-terminate liveness contract.
    // Safety rests on the await_transform whitelist (above): in-tree async
    // awaiters (TaskHandleAwaiter, DroppableSlot::operator co_await) cannot
    // appear inside an EagerChain body, so the suspended frame cannot own a
    // live root join handle. Future addition of any await_transform overload
    // requires re-auditing this claim — any awaitable that holds a strong
    // root resource by value would re-introduce the live-handle-on-frame-
    // destroy termination scenario. Lost results from a destroyed suspended
    // frame surface as implicit cancellation at the await site (defense-in-
    // depth via TaskHandleAwaiter::~ if the whitelist is somehow bypassed).

    EagerChain(EagerChain &&o) noexcept : handle_{std::exchange(o.handle_, {})} {}
    EagerChain &operator=(EagerChain &&o) noexcept {
        if (this != &o) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(o.handle_, {});
        }
        return *this;
    }
    EagerChain(EagerChain const &) = delete;
    EagerChain &operator=(EagerChain const &) = delete;

    // Awaitable: co_await EagerChain<T> inside another EagerChain<U>
    ChainAwaiter<T> operator co_await() &&;

    // Traditional exit: extract the Chain<T>; destroys the coroutine frame.
    // Empty slot means the body suspended (awaited an async awaitable);
    // surface as a Failure-carrying Chain rather than UB-derefing nullopt.
    Chain<T> chain() && {
        auto &p = handle_.promise();
        if (!p.slot_) {
            auto ex = std::make_exception_ptr(root::WorkError{
                "EagerChain suspended: body awaited an asynchronous awaitable"});
            handle_.destroy();
            handle_ = {};
            return Chain<T>{
                root::Outcome<T>{root::Failure{std::move(ex)}},
                CarrierKind::task};
        }
        auto out = std::move(*p.slot_);
        handle_.destroy();
        handle_ = {};
        return Chain<T>{std::move(out), CarrierKind::task};
    }
};

// operator co_await on EagerChain<T> MUST call .chain() first so the empty-slot
// check above runs before the resulting Chain<T> is awaited via 5a's
// ChainAwaiter. Nested EagerChain<U> inside an EagerChain<T> body inherits the
// same protection.
```

Dual-use example:

```cpp
// Coroutine style:
EagerChain<int> process(Chain<int> input) {
    int v = co_await std::move(input);
    co_return v * 2;
}

// Traditional style:
Chain<int> result = process(std::move(chain)).chain();
auto out = std::move(result).release_outcome();
```

The coroutine body runs eagerly and synchronously — no scheduler, no runtime.
One heap allocation per `EagerChain<T>` coroutine invocation (the coroutine
frame); HALO may eliminate it in release builds with inlined call sites.

### 5c: Async Root Handle Awaiter

For coroutines that suspend at a real async boundary, a non-blocking awaiter
wraps a root join handle. Requires R2.

**Return type — `T`, not `Chain<T>`.** `co_await` on a join handle returns the
unwrapped value (or rethrows on failure / cancellation). Returning `Chain<T>`
would force users to write `int v = co_await co_await std::move(h);` which is
ugly and inconsistent with `co_await Chain<T>` (5a) which already returns `T`.
A separate non-coroutine helper `await_chain(handle)` returns `Chain<T>` for
callers who want to feed a chain pipeline.

**Affinity model — critical.** V10 §1541–1544 specifies that `Posted<T>`
resumes on its owner thread. The awaiter below resumes on the producer's
commit thread (whatever thread calls `commit_*`). This is correct for
`Task<T>` (task affinity = resolved, no owner thread) but violates the V10
contract for `Posted<T>` and `Operation<T>`.

To preserve V10 affinity:
- `TaskHandleAwaiter<T>` — wraps `TaskJoinHandle<T>`. Resumes on producer
  thread. V10-correct for tasks.
- `PostedHandleAwaiter<Owner, T>` — wraps `PostedJoinHandle<T>` + owner
  reference. The R2 callback posts `h.resume()` through the owner's execution
  context rather than resuming inline. Requires the owner type to expose
  `post(callable)` or equivalent. If the owner does not support posting,
  provide `TaskHandleAwaiter` only and document that posted handles require
  explicit hop.
- `OperationHandleAwaiter<Driver, T>` — same pattern for `OperationJoinHandle`.

For the initial Phase 5c implementation, ship `TaskHandleAwaiter` only (no
posted/operation affinity). Document that `PostedHandleAwaiter` requires the
owner-posting primitive once that pattern is established.

**API surface — R2 integration.** `handle.control()` returns `BasicControl<Category>`
which holds `shared_ptr<ControlBlockBase>`. R2 adds `try_set_on_ready` to
`ControlBlockBase` and a public forwarder on `BasicControl`. The awaiter uses
the `bool await_suspend` form — return `false` to skip the suspend — so the
already-ready race is handled without inline resumption from inside R2.

```cpp
template<root::work_value T>
class TaskHandleAwaiter {
    root::TaskJoinHandle<T>                              handle_;
    root::BasicControl<root::ControlCategory::task>      control_;  // cached at
                                                                    // ctor — avoids
                                                                    // 2× shared_ptr
                                                                    // copy across
                                                                    // await_ready
                                                                    // + await_suspend
    enum class AwaiterError : std::uint8_t {
        none,
        already_installed,  // single-consumer rule violated
        empty,              // moved-from / default handle
    };
    AwaiterError                 error_ = AwaiterError::none;
    bool                         handle_consumed_ = false;
    bool                         callback_installed_ = false;  // true iff we
                                                               // got `installed`
                                                               // — gates clear_on_ready

public:
    explicit TaskHandleAwaiter(root::TaskJoinHandle<T> &&h) noexcept
        : handle_{std::move(h)}, control_{handle_.control()} {}

    // Defense-in-depth: if the awaiter is destroyed without await_resume
    // consuming the handle (compiler bug, exotic coroutine type,
    // exception thrown out of await_resume mid-extract), reclaim the
    // installed callback (R7 clear_on_ready) ONLY IF WE INSTALLED IT,
    // then abandon the handle to a noop sink. Without the
    // callback_installed_ gate, a defensive dtor on an already_installed
    // path would steal someone else's callback.
    //
    // R7 in_flight: if commit is mid-execution of our installed lambda,
    // we cannot prevent it; downgrade to checked-build diagnostic +
    // best-effort abandon. Soundness limit documented in R2/R7.
    //
    // Normal completion paths set handle_consumed_ = true and skip this.
    ~TaskHandleAwaiter() noexcept {
        if (handle_consumed_ || !bool(handle_)) return;  // R6 operator bool

        if (callback_installed_) {
            auto status = control_.clear_on_ready();
            if (status == root::ClearOnReadyStatus::in_flight) {
#ifdef CONFLUX_WORK_CHECKED_BUILD
                // Log handle address (or coroutine handle, if recoverable)
                // so the racing awaiter is identifiable from a checked-build
                // crash log.
                root::emit_carrier_diagnostic_fmt(
                    "TaskHandleAwaiter dtor raced commit's in-flight callback "
                    "— UB possible if coroutine frame is also being destroyed "
                    "(awaiter=%p)", static_cast<void *>(this));
#endif
                // Best-effort: still abandon below; commit's lambda dispatch
                // will race coroutine teardown if it has not finished.
            }
        }
        // try_abandon_to is idempotent w.r.t. already-abandoned control
        // blocks: install_abandon_sink would terminate, so MUST go through
        // the try_ form here.
        (void) root::try_abandon_to(std::move(handle_),
                                    root::drop_on_abandon{});
#ifdef CONFLUX_WORK_CHECKED_BUILD
        root::emit_carrier_diagnostic(
            "TaskHandleAwaiter destroyed unconsumed — defensive abandon");
#endif
    }

    [[nodiscard]] bool await_ready() const noexcept {
        return control_.ready();
    }

    // bool form: false → coroutine does NOT suspend (resume immediately).
    // This eliminates the await_ready/await_suspend race window without
    // requiring R2 to ever invoke the callback inline.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        auto result = control_.try_set_on_ready(
            [h]() mutable noexcept { h.resume(); });
        switch (result.status) {
            case root::ReadyRegistration::installed:
                callback_installed_ = true;   // gate dtor's clear_on_ready
                return true;                  // suspend; callback will resume
            case root::ReadyRegistration::already_ready:
                return false;                 // skip suspend; await_resume joins (non-blocking)
            case root::ReadyRegistration::already_installed:
                error_ = AwaiterError::already_installed;
                return false;                 // skip suspend; await_resume throws
            case root::ReadyRegistration::empty:
                error_ = AwaiterError::empty;
                return false;                 // skip suspend; await_resume throws
        }
        return false;
    }

    T await_resume() {
        // CRITICAL: handle ownership must be resolved BEFORE any throw;
        // do NOT set handle_consumed_ early — the dtor relies on it for
        // defensive abandon if we throw mid-extract.
        if (error_ == AwaiterError::already_installed) {
            // Misuse path — duplicate consumers attached to the same control
            // block. Callback belongs to another consumer; do NOT
            // clear_on_ready (that would steal it). Abandon our copy of the
            // handle through try_abandon_to (idempotent vs already-abandoned
            // sinks the winning consumer may have installed), then throw.
            // The state of the original work is NOT guaranteed for the
            // losing consumer; document in D1 alongside JoinContextReason.
            (void) root::try_abandon_to(std::move(handle_),
                                        root::drop_on_abandon{});
            handle_consumed_ = true;
            throw root::JoinContextError{
                "TaskHandleAwaiter: another consumer already installed the on-ready hook",
                root::JoinContextReason::ready_callback_already_installed};
        }
        if (error_ == AwaiterError::empty) {
            handle_consumed_ = true;       // bool(handle_) is already false
            throw root::JoinContextError{
                "TaskHandleAwaiter: moved-from or default handle awaited",
                root::JoinContextReason::thread_precondition};
        }
        // Terminal published (R2 gate semantic). root::join is non-blocking.
        auto out = root::join(std::move(handle_));
        handle_consumed_ = true;
        if (out.is_success()) {
            if constexpr (std::same_as<T, void>) return;
            else return std::move(out).success().value;
        }
        if (out.is_failure())
            std::rethrow_exception(std::move(out).failure().error);
        throw root::CancelledError{out.cancelled().reason};
    }
};

template<root::work_value T>
[[nodiscard]] TaskHandleAwaiter<T> operator co_await(
    root::TaskJoinHandle<T> &&jh) noexcept;

// Carrier-style helper (returns Chain<T> not T) for pipelines.
// Same suspend semantics as TaskHandleAwaiter (R2 try_set_on_ready);
// await_resume returns Chain<T> constructed from root::join's outcome
// rather than rethrowing. Useful when feeding the result into another
// carrier combinator (map/then/when_all) inside a coroutine body.
template<root::work_value T>
class TaskHandleChainAwaiter {
    root::TaskJoinHandle<T>                              handle_;
    root::BasicControl<root::ControlCategory::task>      control_;
    enum class AwaiterError : std::uint8_t { none, already_installed, empty };
    AwaiterError                        error_ = AwaiterError::none;
    bool                                handle_consumed_ = false;
    bool                                callback_installed_ = false;
public:
    explicit TaskHandleChainAwaiter(root::TaskJoinHandle<T> &&h) noexcept
        : handle_{std::move(h)}, control_{handle_.control()} {}

    // Symmetric defensive abandon — same shape as ~TaskHandleAwaiter.
    // Same callback_installed_ ownership gate; same in_flight soundness
    // limit applies.
    ~TaskHandleChainAwaiter() noexcept {
        if (handle_consumed_ || !bool(handle_)) return;
        if (callback_installed_) {
            auto status = control_.clear_on_ready();
            if (status == root::ClearOnReadyStatus::in_flight) {
#ifdef CONFLUX_WORK_CHECKED_BUILD
                root::emit_carrier_diagnostic_fmt(
                    "TaskHandleChainAwaiter dtor raced commit's in-flight callback "
                    "(awaiter=%p)", static_cast<void *>(this));
#endif
            }
        }
        (void) root::try_abandon_to(std::move(handle_), root::drop_on_abandon{});
#ifdef CONFLUX_WORK_CHECKED_BUILD
        root::emit_carrier_diagnostic(
            "TaskHandleChainAwaiter destroyed unconsumed — defensive abandon");
#endif
    }

    [[nodiscard]] bool await_ready() const noexcept { return control_.ready(); }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept;

    // await_resume — pipeline-friendly: registration errors and producer
    // failures surface as Failure-carrying Chain<T> rather than throws.
    // NOT noexcept: make_exception_ptr and Chain construction can allocate;
    // an OOM would terminate inside a noexcept frame, which is worse than
    // propagating to the coroutine's caller.
    Chain<T> await_resume() {
        if (error_ == AwaiterError::already_installed) {
            // Do NOT clear_on_ready — callback belongs to another consumer.
            // try_abandon_to: idempotent vs already-abandoned sinks the
            // winning consumer may have installed.
            (void) root::try_abandon_to(std::move(handle_),
                                        root::drop_on_abandon{});
            handle_consumed_ = true;
            auto ex = std::make_exception_ptr(root::JoinContextError{
                "TaskHandleChainAwaiter: another consumer already installed the on-ready hook",
                root::JoinContextReason::ready_callback_already_installed});
            return Chain<T>{root::Outcome<T>{root::Failure{std::move(ex)}},
                            CarrierKind::task};
        }
        if (error_ == AwaiterError::empty) {
            handle_consumed_ = true;
            auto ex = std::make_exception_ptr(root::JoinContextError{
                "TaskHandleChainAwaiter: moved-from or default handle awaited",
                root::JoinContextReason::thread_precondition});
            return Chain<T>{root::Outcome<T>{root::Failure{std::move(ex)}},
                            CarrierKind::task};
        }
        // Terminal published per R2 gate semantic; root::join is non-blocking.
        auto out = root::join(std::move(handle_));
        handle_consumed_ = true;
        return Chain<T>{std::move(out), CarrierKind::task};
    }
};

// await_suspend mirrors TaskHandleAwaiter — sets callback_installed_ true
// only on `installed` status, error_ on already_installed/empty.

// Asymmetry table (REQUIRED in D1):
//
// | Path                     | TaskHandleAwaiter            | TaskHandleChainAwaiter   |
// |--------------------------|------------------------------|--------------------------|
// | Success                  | returns T                    | returns Chain<T>{Success}|
// | Producer failure         | rethrows producer's exception| returns Chain<T>{Failure}|
// | Producer cancellation    | throws CancelledError        | returns Chain<T>{Cancelled}|
// | already_installed reg.   | throws JoinContextError      | returns Chain<T>{Failure(JCE)}|
// | empty handle reg.        | throws JoinContextError      | returns Chain<T>{Failure(JCE)}|
// | noexcept declared?       | NO (rethrows)                | NO (allocates)           |

template<root::work_value T>
[[nodiscard]] TaskHandleChainAwaiter<T>
await_chain(root::TaskJoinHandle<T> &&jh) noexcept;

// Cross-thread liveness: same as TaskHandleAwaiter — resumes on producer's
// commit thread (V10-correct for tasks). Posted/Operation variants require
// owner-affine resumption (deferred; same constraint as 5c).

// Deleted overloads for Posted/Operation handles. Pair `= delete` with
// `[[deprecated(message)]]` so the deletion diagnostic carries the
// actionable hint at the call site (clang/gcc surface the deprecated
// message even on `= delete`).
// Scope::admit is BLOCKING (carrier_scope.cxx:120 calls root::join
// synchronously). Calling it from inside a coroutine body would block
// the coroutine's thread — defeating the purpose. The deprecation
// messages name the non-coroutine context explicitly to prevent that
// foot-gun.
template<root::work_value T>
[[deprecated("co_await on PostedJoinHandle requires owner-affine resumption "
             "(not yet implemented). FROM A NON-COROUTINE CONTEXT use "
             "Scope::admit to obtain a Chain<T> (admit BLOCKS — do NOT call "
             "from inside a coroutine), then co_await the Chain.")]]
auto operator co_await(root::PostedJoinHandle<T>&&) = delete;

template<root::work_value T>
[[deprecated("co_await on OperationJoinHandle requires driver-affine "
             "resumption (not yet implemented). FROM A NON-COROUTINE CONTEXT "
             "use Scope::admit to obtain a Chain<T> (admit BLOCKS — do NOT "
             "call from inside a coroutine), then co_await the Chain.")]]
auto operator co_await(root::OperationJoinHandle<T>&&) = delete;
```

Exit criteria:
- `Chain<T>::operator co_await()` compiles; Phase 4 tests pass unchanged
- `EagerChain<T>` round-trips through all Phase 1–4 carrier combinators
- coroutine and traditional paths produce identical outcomes for same inputs
- `EagerChain<T>` perf gate: ≤ one coroutine-frame allocation + 20% overhead
  vs equivalent hand-written function (NOT vs `carrier_a/single_hop` —
  HALO is not portable across compilers; do not gate on its presence). For
  hot paths, traditional combinators remain recommended.
- R2 prerequisite: stress test for `try_set_on_ready` / `commit_*` race +
  `bool await_suspend` correctness (see R2 exit criteria; specifically that
  `already_ready` does NOT cause inline resume from inside `try_set_on_ready`)
- `TaskHandleAwaiter::await_ready()` short-circuits correctly when result is
  already available before `await_suspend` is entered
- `co_await TaskJoinHandle<int>` returns `int` (not `Chain<int>`); test with
  success, failure, cancel branches
- `co_await posted_handle` fails to compile with the `[[deprecated]]`
  message naming `Scope::admit` as the remediation
- `await_chain(handle)` round-trips through carrier combinators (map/then)
  inside a real **async coroutine** (root `Task<T>` coroutine or a future
  AsyncChain<T>) — NOT inside an EagerChain body. The EagerChain
  await_transform whitelist deliberately deletes async awaiters, so
  await_chain inside EagerChain MUST fail to compile (test this — pin
  the sync-only safety guarantee)
- `~TaskHandleAwaiter` on an unconsumed live handle does NOT terminate
  (defensive abandon test — destroy the awaiter via thrown exception inside
  await_ready / await_suspend, verify abandon path runs)

---

## Phase 6: Carrier Completions

Small items deferred from Phase 4 reviews. Fully additive, no design risk.

### `hop_to_task` / `unbind`

Symmetric with `hop_to_posted` / `hop_to_operation`.

```cpp
// Resets kind to task and clears bound_cap_:
template<root::work_value T>
[[nodiscard]] Chain<T> hop_to_task(Chain<T> &&chain) noexcept;

// Clears bound_cap_ without changing kind:
template<root::work_value T>
[[nodiscard]] Chain<T> unbind(Chain<T> &&chain) noexcept;
```

### `Scope::admit` Auto-bind + `admit_unbound` Opt-out

`admit(Owner&, PostedJoinHandle<T>&&)` already receives the owner capability.
Return a `Chain<T>` pre-bound to `capability_id(owner)` by default. Add an
explicit `admit_unbound` opt-out for the rare case.

```cpp
template<class Owner, root::work_value T>
[[nodiscard]] Chain<T> admit(Owner &owner,
                             root::PostedJoinHandle<T> &&jh);          // bound
template<class Owner, root::work_value T>
[[nodiscard]] Chain<T> admit_unbound(Owner &owner,
                                     root::PostedJoinHandle<T> &&jh);  // explicit no-bind
```

`unbind(scope.admit(...))` continues to work, but `admit_unbound` reads as
intent at the call site rather than as a remediation.

**Ergonomic justification:** the caller already passes `Owner&`; requiring a
separate `hop_to_posted` call every time to bind a capability that `admit`
already has is redundant. The common case (admit and stay on this owner) is
the default. The rare case (admit but do not bind) reaches for `admit_unbound`.

**Behaviour change:** after auto-bind, `verify_hop(other_cap, chain)` on an
`admit` result throws `HopCapabilityError` where it previously passed (since
`bound_cap_` was null, the check short-circuited). This is the intended
strengthening — callers must audit hop sites that follow `admit`.

This changes the Phase 2 `admit` return contract. **Self-contradicts the
"carrier additive-only" rule (§Design Principles); the principles section
explicitly notes this exception. Do not relax further.**

Gating steps:
1. Before merge: `grep -rn 'bound_capability\|admit' tests/ src/` — inspect
   every `address == nullptr` assertion on `admit`'s return value AND every
   `verify_hop` call following an `admit` result. Document each one that needs
   updating. Also scan downstream consumer trees if any exist.
2. Add a Catch2 regression test that calls `verify_hop(other_cap, scope.admit(owner, jh))`
   and asserts `HopCapabilityError`. This pins the new strict semantics so a
   future "fix" cannot silently weaken auto-bind back to nullptr.
3. Brief benchmark confirming the extra `capability_id()` call in `admit` is
   within noise on `carrier_a/scope_admit` (Phase 1 benchmark — file the
   benchmark first if it does not yet exist).

### `into_ready_task(Chain<T>)` (G3)

A caller who has a `Chain<T>` cannot pass it to a function expecting
`root::Task<T>` without a re-wrap. After Phase 5a, they can `co_await` the
chain inside their own coroutine, but they cannot hand it off to a library
that takes a `Task<T>` parameter directly. `Scope::admit` therefore imposes a
refactoring tax on every downstream signature.

**Naming:** `into_ready_task` (NOT `into_task`). The name must signal that the
operation creates a fresh root control-block — it is a real allocation, not a
free type pun. `into_task` suggested a structural cast and was misleading.

```cpp
// carrier_model_a.cxx — adds ~20 lines
template<root::work_value T>
[[nodiscard]] root::Task<T> into_ready_task(Chain<T> &&chain);
```

Implementation: `make_task_source<T>(SubmitOptions{.enable_cancellation = false})`,
inspect `release_outcome()`, commit the matching arm (`commit_success` /
`commit_failure` / `commit_cancelled`), return the `Task<T>`. Using
`enable_cancellation = false` minimises control-block size; the chain's outcome
is already resolved so cancellation is moot.

This restores composability between the carrier layer and root-typed call sites
without a semantic bridge — it is a one-shot terminal conversion, not a
persistent shim. **NOT a hot-path operation** (allocates a control block);
document this prominently in D1.

This bridge is the same shape as the §G2a-banned `Flow<T>::into_root_task()` —
the difference is direction (carrier → root vs legacy → root) and resolved-state
precondition (chain is already terminal). G2a is still banned. `into_ready_task`
is permitted because the carrier layer is the future API surface, not legacy.

Exit criteria:
- `into_ready_task()` produces a `Task<T>` whose `root::join` returns an
  equivalent outcome for all three branches (success, failure, cancelled)
- `into_ready_task()` on a `Chain<T>` returned from `Scope::admit` (carries
  `bound_cap_` after Phase 6 auto-bind) drops the bound capability — the
  resulting `Task<T>` is task-category, has no capability, and `can_join`
  with any owner returns true. Test pins the drop semantics so a future
  refactor cannot silently propagate `bound_cap_` into a task.

### `when_all` Multi-Failure Aggregation Policy

`carrier_model_a.cxx:154–177` currently combines two `when_all` arms by
checking arm A's outcome first and returning A's failure on any A failure;
B's exception is silently dropped on dual-failure. This is a data-loss bug
masquerading as a default policy.

**Decision:** introduce `AggregateError` as the explicit dual-failure type.
Single-failure cases continue to rethrow the original exception unchanged
(no behaviour change for the common case).

```cpp
// carrier_model_a.cxx — adds ~30 lines
class AggregateError : public root::WorkError {
public:
    explicit AggregateError(std::vector<std::exception_ptr> causes);

    // Lifetime-safe accessor — returns a stable copy. Use this for
    // capture-heavy callback patterns where the AggregateError may be
    // moved between observation and use.
    [[nodiscard]] std::vector<std::exception_ptr> causes_owned() const;

    // View-only accessor — span lifetime is bound to *this. Cheaper but
    // dangerous: moving the AggregateError invalidates the span. Marked
    // [[nodiscard]] so accidental discard is caught; consider this the
    // expert-only path.
    [[nodiscard("span lifetime bound to *this — moves invalidate")]]
    std::span<std::exception_ptr const> causes_view() const noexcept;
private:
    std::vector<std::exception_ptr> causes_;
};
```

**Earlier draft exposed only `causes()` returning `std::span` — that's a
foot-gun-prone API for the common "store the AggregateError in a callback
capture" pattern.** Renamed to `causes_view()` with explicit warning;
added `causes_owned()` as the safe default. D1 must promote
`causes_owned()` as the recommended accessor and show the moved-AggregateError
case as a hazard for `causes_view()`.

`when_all(a, b)` outcome priority — **matches current carrier_model_a.cxx:154–177
behaviour (failure beats cancelled on the same arm; A checked before B);**
spec amended to match the code rather than the reverse:

- A success, B success → success
- A failure, B success → rethrow A's cause (current behaviour)
- A success, B failure → rethrow B's cause (current behaviour)
- A failure, B failure → throw `AggregateError{a_cause, b_cause}` (new — fixes
  the silent drop of B's exception)
- A failure, B cancelled → rethrow A's cause (failure beats cancelled today)
- A cancelled, B failure → rethrow B's cause (failure beats cancelled today)
- A cancelled, B cancelled → cancelled with first-arm reason
- A success, B cancelled → cancelled with B's reason
- A cancelled, B success → cancelled with A's reason

**Documented priority:** `failure (single or aggregate) > cancelled > success`.
Earlier draft asserted "any cancelled → cancelled wins"; that was wrong vs
implementation. If the implementation should change to cancelled-first, file
as a separate behavioural change with its own decision record — do NOT bundle
into Phase 6.

`AggregateError : root::WorkError` so existing `catch (WorkError &)` sites
continue to catch dual-failure cases. Callers wanting the original cause
on the single-failure path see no change.

Exit criteria:
- `AggregateError` catchable as `WorkError` and `std::exception`
- `when_all` dual-failure test produces `AggregateError` with both causes
  visible via `causes()`
- `when_all` single-failure test produces the original cause unchanged (no
  `AggregateError` wrapping)
- `when_all(A cancelled, B failure)` rethrows B's failure (NOT cancellation)
  — pins the documented `failure > cancelled` priority; regression-guards
  any future "fix" that silently flips the precedence
- `when_all(A failure, B cancelled)` rethrows A's failure (symmetric)
- `AggregateError::causes_owned()` returns a stable `vector<exception_ptr>`
  copy; safe to capture across moves of the AggregateError
- `AggregateError::causes_view()` returns a span; test that moving the
  AggregateError invalidates the previously-obtained span (ASAN run);
  D1 explicitly recommends `causes_owned()` as default
- N-ary `when_all` (when added) follows the same rule: 2+ failures → aggregate

### `HopCapabilityError` Reparented + Reason

With R1 landed:

```cpp
class HopCapabilityError final : public root::JoinContextError {
public:
    HopCapabilityError()
        : JoinContextError{"carrier: hop capability mismatch",
                           root::JoinContextReason::hop_capability_mismatch} {}
};
```

Callers that catch `root::JoinContextError` now automatically catch carrier hop
misuse alongside root join misuse.

Exit criteria:
- `hop_to_task` + `unbind` match Phase 4 test patterns
- `Scope::admit` auto-bind: all Phase 2 tests pass unchanged or with trivial
  updates; grep step documented above is performed before merge
- benchmark diff within noise on `carrier_a/mixed_3stage`
- `HopCapabilityError` catchable as `JoinContextError` and `WorkError`

---

## Phase 7: `std::execution` Adapter Layer

### Goal

Wrap root result handles and join handles as C++26 `std::execution` senders.
Root types remain non-senders. The adapter layer is purely additive in a new
module (`conflux.work.carrier.exec`).

### Design

```cpp
// conflux.work.carrier.exec

template<root::work_value T>
class TaskSender {
    root::Task<T> task_;
public:
    using completion_signatures = std::execution::completion_signatures<
        std::execution::set_value_t(T),
        std::execution::set_error_t(std::exception_ptr),
        std::execution::set_stopped_t()>;

    template<std::execution::receiver R>
    auto connect(R &&r) && -> /* operation_state */;
};

template<root::work_value T>
[[nodiscard]] TaskSender<T> as_sender(root::Task<T> &&task);

// Same for posted/operation:
template<root::work_value T>
[[nodiscard]] /* PostedSender<T> */ as_sender(root::Posted<T> &&posted);

// Chain<T> sender: start() completes synchronously; no thread, queue, or
// allocation beyond what the receiver requires.
template<root::work_value T>
[[nodiscard]] /* ChainSender<T> */ as_sender(Chain<T> &&chain);
```

Root handles use R2 (`try_set_on_ready` on `ControlBlockBase`) to connect
terminal commit to receiver notification. The operation state installs the
callback in `start()` and dispatches `set_value` / `set_error` /
`set_stopped` when it fires. `try_set_on_ready` returning `already_ready` is
handled the same way as 5c: dispatch the receiver notification on the calling
thread (which IS `start()`'s thread), not from inside R2's protocol. Returning
`already_installed` is a programming error — sender connections are
single-consumer; debug-assert and surface as `set_error(JoinContextError{
ready_callback_already_installed})` in release.

**Affinity note:** same constraint as 5c. `PostedSender<T>` must post the
receiver notification through the owner's execution context to satisfy V10
affinity. The operation state must hold an owner reference or executor token.
Design detail deferred to the implementation phase.

Exit criteria:
- `as_sender(Chain<T>)` satisfies `std::execution::sender` concept
- `as_sender(root::Task<T>)` compiles and passes a `sync_wait` round-trip test
- Static assert: `root::Task<T>` does not satisfy `std::execution::sender`
- No regression on existing carrier benchmarks

---

## Phase 8: Droppable / Coalescing Streams

### Goal

Add late-data drop and latest-wins policies as separate stream primitives
above the carrier layer.

### Design Sketch

**Naming:** `DroppableSlot<T>` — not `DroppableHandle<T>`. "Handle" implies
the root liveness contract (destructor terminates). This type explicitly does
not have that contract, so "Handle" is wrong. "Slot" communicates a receptacle
that may or may not be filled and can be abandoned.

**What DroppableSlot holds:** an in-flight result — a `TaskJoinHandle<T>`
(or equivalent) plus an R2 `on_ready` registration. The underlying root work
continues even if the slot is dropped. "Drop" means the consumer discards
the result slot; the result itself arrives at the producing source normally
and its outcome is consumed by a drain hook installed by the destructor
(see below). The *consumer-side* destructor is benign; the *producer side*
still has its liveness contract — `DroppableSlot` does not bypass it, it
satisfies it via the drain hook.

**Root-liveness drain — load-bearing.** The destructor MUST satisfy the root
join handle's liveness contract WITHOUT blocking. Blocking destructors in
async systems are worse than noisy failure (Review 3). The mechanism:

| Slot state at `~DroppableSlot` | Action |
|---|---|
| Already consumed via `try_get`/`wait`/`co_await` | nothing; handle was already moved out |
| Ready, not consumed | invoke `on_drop_fn_(outcome)` if installed; consume outcome via `root::join` to satisfy liveness; do NOT terminate |
| Not yet ready | call `try_set_on_ready` with a self-contained drain lambda that **owns the moved-out join handle by value** (NOT a `shared_ptr<ControlBlock>` — see cycle discussion below). Lambda calls `root::join` on the owned handle FIRST (consumes outcome, satisfies liveness), THEN invokes `on_drop_fn_(outcome)` if installed. Reorder is load-bearing: if `on_drop_fn_` runs first and throws, the unwind destroys the still-live handle → `~TaskJoinHandle` terminate. `on_drop_fn_` is concept-required to be noexcept (see below); the consume-first order is defense in depth. Lambda is heap-allocated by `MoveOnlyFunction` if it exceeds SBO; the control block keeps the lambda alive until terminal commit. |

**Drain pre-allocation shape — load-bearing.** The drain lambda must own
the moved-out join handle by value, but the slot needs the handle alive
until consumption (`try_get`/`co_await`/`wait`). Two options were
considered:

- **Option A (rejected):** preallocate erased-callable storage only; emplace
  the actual lambda at destruction time. Rejected because the in-tree
  `MoveOnlyFunction` does not expose a placement-construct surface, and
  adding one is a separate root change.
- **Option B (chosen):** the slot owns a `unique_ptr<DrainState>` allocated
  at construction. Destructor moves the state into the on-ready callable
  via a thin lambda that captures `unique_ptr<DrainState>` (which fits SBO
  trivially — single pointer), avoiding any allocation in the destructor.

```cpp
template<root::work_value T>
struct DrainState {
    root::TaskJoinHandle<T>                              handle;
    MoveOnlyFunction<void(root::Outcome<T>)>             on_drop_fn;  // empty by default
};

// This is the IMPLEMENTATION sketch showing internal storage shape and
// dtor logic. The PUBLIC API is defined in the next code block below
// (with `try_get`, `wait`, `operator co_await`, concept-constrained
// `on_drop`). Both refer to the same class; this version emphasises
// the unique_ptr<DrainState> + ~dtor flow, the next version emphasises
// the public surface.
template<root::work_value T>
class DroppableSlot {
    std::unique_ptr<DrainState<T>> state_;          // allocated at admit time
    bool                           consumed_ = false;
public:
    explicit DroppableSlot(root::TaskJoinHandle<T> &&h)
        : state_{std::make_unique<DrainState<T>>(DrainState<T>{std::move(h), {}})} {}

    // Public surface (try_get / wait / on_drop / operator co_await) — see
    // canonical definition below; all operate on state_->handle.

    ~DroppableSlot() noexcept {
        if (consumed_ || !state_) return;
        // Cache the control handle BEFORE moving state_ into the lambda;
        // earlier draft did `state_->handle.control().try_set_on_ready(...)`
        // AFTER moving state_ — null deref UB. Cache first, move second.
        auto control = state_->handle.control();

        if (control.ready()) {
            // Ready path: extract outcome inline, fire on_drop, consume handle.
            // No try_set_on_ready needed.
            auto out = root::join(std::move(state_->handle));
            if (state_->on_drop_fn) state_->on_drop_fn(std::move(out));
            return;
        }

        // Unready path: install drain hook. Lambda captures unique_ptr<DrainState>
        // by move — single pointer, fits SBO, no allocation in the dtor.
        auto drain = MoveOnlyFunction<void()>{
            [s = std::move(state_)]() mutable noexcept {
                // R2 callback fires on commit thread. join is non-blocking
                // (R2 gate semantic).
                auto out = root::join(std::move(s->handle));   // consume FIRST
                if (s->on_drop_fn) s->on_drop_fn(std::move(out));
            }};

        auto result = control.try_set_on_ready(std::move(drain));
        switch (result.status) {
            case root::ReadyRegistration::installed:
                return;            // commit will fire the drain
            case root::ReadyRegistration::already_ready:
                // Outcome was published between ready() check and try_set_on_ready.
                // The drain lambda was returned in rejected_fn; invoke it inline
                // so it consumes the handle and fires on_drop_fn. Without this,
                // the lambda dies still owning the handle → ~TaskJoinHandle
                // terminates.
                if (result.rejected_fn) result.rejected_fn();
                return;
            case root::ReadyRegistration::already_installed:
                // Single-consumer contract violation.
#ifdef CONFLUX_WORK_CHECKED_BUILD
                root::emit_carrier_diagnostic(
                    "DroppableSlot: single-consumer rule violated — drain hook "
                    "could not install");
#endif
                std::terminate();
            case root::ReadyRegistration::empty:
                return;            // handle already moved out elsewhere
        }
    }
};
```

**Slot footprint:** `sizeof(DroppableSlot<T>) = sizeof(unique_ptr) + bool +
padding ≈ 16 bytes`. The DrainState heap allocation is the cost the user
pays for the non-blocking, terminate-free destructor.

**Cycle analysis — root must cooperate.** Naive ownership:
```
ControlBlock --owns--> on_ready_fn_ (lambda)
   ^                          |
   |                         owns
   |                          v
   +---owned-by--- TaskJoinHandle (strong ref)
```
This is a true cycle. It is broken ONLY if either (a) terminal commit fires,
which clears `on_ready_fn_` and drops the handle, or (b) the producer's
abandon path actively clears or fires `on_ready_fn_`.

**Required root contract (D3 addition):** the producer-side abandonment path
MUST clear any installed `on_ready_fn_` (drop the lambda → drop the handle
→ break the cycle), OR transition the control block to a terminal state and
fire the callback as part of abandon. Without this guarantee, dropping a
DroppableSlot whose producer is later abandoned without commit leaks the
control block forever. Document in D3 alongside the existing commit/cancel
race text. Test: drop unready slot → abandon producer (no commit) → assert
control block reclaimed (LeakSanitizer).

If root cannot guarantee abandon-clears-on_ready (e.g., for legacy paths),
the drain lambda MUST use a detached-drain primitive (`root::detach_drain(handle)`)
that registers as a non-owning observer rather than holding a strong handle
reference. That primitive does not exist today — file as a follow-up if the
abandon-clears guarantee turns out to be hard.

**Single-consumer hard contract.** `DroppableSlot` is **single-consumer, not
thread-safe.** `try_get`, `wait`, `co_await`, `on_drop`, and the destructor
MUST NOT race with each other. R2's `try_set_on_ready` enforces one-callback
lifetime; combining `co_await` with destructor-drain on the same slot would
violate it. Document loudly in D1.

If `try_set_on_ready` returns `already_ready` (raced with commit), destructor
takes the ready path above. If it returns `already_installed` (someone else
hooked it — violation of single-consumer rule), debug build asserts; release
build calls `std::terminate` with a diagnostic message naming the slot. Do
NOT fall back to a blocking `root::join` — a destructor that can block
indefinitely is a sharper foot-gun than a loud abort.

This is **option B** from the design discussion: `on_drop` observes late
outcomes asynchronously, which is the useful semantics for HTTP late-data
patterns and telemetry streams. Option A (only ready-at-drop observed) is
explicitly rejected.

**Dropped failures with no `on_drop` installed:** silently discard the
exception (the drain lambda still calls `root::join` to satisfy liveness; the
outcome is then dropped). This is by design for late-data-drop patterns.
`CONFLUX_WORK_CHECKED_BUILD` (Phase 10b) emits a diagnostic in this case.

```cpp
// conflux.work.carrier.streams

// Public API surface — see implementation sketch above for storage shape
// and ~dtor body. Both code blocks describe the same class; this one
// emphasises the user-facing operations.
template<root::work_value T>
class DroppableSlot {
public:
    // Storage: unique_ptr<DrainState<T>> (handle + optional on_drop_fn_).
    // See implementation sketch above.

    // Distinct from root types: destructor does NOT terminate for the
    // common drop-before-consume case AND does NOT block. Drain hook
    // installed at dtor; lambda captures unique_ptr<DrainState> (single
    // pointer, fits MoveOnlyFunction SBO — no allocation in dtor).
    // Single-consumer slot — racing dtor with try_get/wait/co_await is
    // UB / debug-asserted / release-terminates.
    ~DroppableSlot() noexcept;

    // Install a drop observer. Called at most once, on destructor thread,
    // only if the result was not consumed via try_get()/wait()/co_await.
    // No-op if result was already consumed.
    //
    // CONTRACT: fn must be noexcept-callable. The drain lambda runs inside
    // a noexcept commit-thread callback; throwing inside fn would propagate
    // into a noexcept frame → terminate. If fn can fail, the user must
    // wrap with try/catch internally and decide its own policy.
    template<class F>
        requires std::invocable<F, root::Outcome<T>>
              && std::is_nothrow_invocable_v<F, root::Outcome<T>>
    void on_drop(F &&fn) noexcept;

    // Check if result is ready without blocking:
    [[nodiscard]] bool ready() const noexcept;

    // Consume result if ready; nullopt if not yet available (non-blocking):
    [[nodiscard]] std::optional<root::Outcome<T>> try_get() &&;

    // Block until ready; result is a Chain<T>:
    [[nodiscard]] Chain<T> wait() &&;

    // co_await gives Chain<T> when ready:
    auto operator co_await() && -> /* awaiter */;
};

// CoalescingSlot<T>: latest-wins delivery point.
template<root::work_value T>
class CoalescingSlot {
public:
    void commit(T value) noexcept;
    [[nodiscard]] std::optional<T> take() noexcept;
    [[nodiscard]] bool available() const noexcept;
};
```

Debug-build note: when `CONFLUX_WORK_CHECKED_BUILD` (Phase 10b) is enabled,
dropped `Failure` outcomes that have no `on_drop` installed emit a diagnostic
before discarding, to aid development.

Drop/coalescing metrics hooks deferred to Phase 9.

Exit criteria:
- `DroppableSlot<T>` destructor does not terminate on drop (Catch2 test)
- `on_drop` fires exactly once with the outcome when slot is dropped unconsumed
  AND result was already ready
- `on_drop` fires exactly once with the late outcome when slot is dropped
  before ready (drain-hook async observer test — option B semantics)
- `on_drop` does NOT fire when result was consumed via `try_get`/`wait`/`co_await`
- Drain-hook test: drop unready slot, complete the producer, verify the
  underlying control block reaches terminal state without `std::terminate`
  (ASAN run; LeakSanitizer run — no leaked control blocks)
- Drain-hook lifetime test: drop unready slot, **abandon the producer
  WITHOUT commit**, verify no leak (lambda's owned handle drops on producer
  abandon path; control block reclaimed) — the no-cycle invariant
- Single-consumer assertion test: install `on_drop`, also `co_await` the
  same slot; debug build asserts; release build terminates with diagnostic
  rather than blocks
- `CoalescingSlot<T>` single-producer single-consumer correctness under
  concurrent `commit` + `take`
- `sizeof(DroppableSlot<T>)` recorded; compared to `sizeof(TaskJoinHandle<T>)
  + MoveOnlyFunction<void(Outcome<T>)>` baseline. The `MoveOnlyFunction` of
  the drain lambda lives inside the control block, NOT inside `DroppableSlot`
  itself, so the slot itself stays small. Slot size delta should be
  `sizeof(MoveOnlyFunction<void(Outcome<T>)>)` (the on_drop_fn_) plus the
  pre-allocated drain-lambda storage referenced via pointer (~16 bytes).
- `bench/droppable_slot_drop_unready` micro-bench: drop unready slot, fire
  producer commit, measure end-to-end drain cost. **Initial measurement
  target** (NOT a hard gate until first benchmark data exists): ~200 ns
  p50, ~500 ns p99. End-to-end includes `try_set_on_ready`, lambda
  invocation on commit thread, `root::join`, optional observer call —
  realistic numbers depend on hardware. Convert to a hard gate after
  baseline measurement.
- `bench/droppable_slot_drop_ready` micro-bench: drop slot whose result is
  already terminal. Should not allocate. **Initial target**: ~150 ns p50
  (revised up from 50 ns — destructor must take `outcome_mtx_` via
  `root::join` extraction; uncontended mutex pair alone is ~20-40 ns).
  Convert to hard gate after baseline + 50% slack.
- `on_drop` example must be in D1: `slot.on_drop([](root::Outcome<T> out)
  noexcept { ... });` — the explicit `noexcept` lambda spec is required by
  the concept; without an example, users will hit confusing concept errors.
- `AggregateError::causes()` lifetime test: capture span, move the
  AggregateError, assert original span is invalidated (use ASAN). Document
  in D1 alongside the `causes()` accessor with a "valid for the
  AggregateError's lifetime; do not store across moves" warning. Consider
  also exposing `causes_owned()` returning `std::vector<exception_ptr>`
  by-value for callers that need a stable copy.

---

## Phase 8.5: Per-Lane Timer Service (G4)

### Problem

`DeadlineScope` (`carrier_deadline.cxx`) spawns one `std::jthread` per
instance. At N concurrent requests with K phases each, this is K×N parked
kernel threads. For HTTP at 1000 concurrent requests with 6 deadline phases
each: 6000 jthreads. Unacceptable.

`DeadlineScope` is still correct and useful for callers that live outside an
I/O lane (a normal thread wanting a one-shot deadline). It is the wrong tool
for I/O-heavy consumers.

**Short-term rule (immediate):** I/O-heavy consumers (HTTP, file I/O, TLS)
must NOT use `DeadlineScope` for per-phase timeouts. Instead: track
`steady_clock::now() + budget` and check inline at each await point inside the
per-phase loop.

### Long-Term Design

Add a per-lane timer service backed by `timerfd_create` on Linux. One fd per
lane, one min-heap of pending deadlines.

```cpp
// conflux.work.carrier.timer (new module)

// Installed on a RingLane (or equivalent I/O driver):
struct TimerService;

// Factory: borrow a one-shot timeout scoped to a lane's timer service.
// Cancels automatically if destroyed before deadline.
template<class Clock = std::chrono::steady_clock>
class LaneTimerScope {
public:
    // Fires cancel_fn when deadline elapses (on the lane's thread).
    LaneTimerScope(TimerService &svc,
                   Clock::time_point deadline,
                   MoveOnlyFunction<void()> cancel_fn);

    // Destructor removes the pending deadline entry; no-op if already fired.
    ~LaneTimerScope() noexcept;
};
```

`DeadlineScope` remains unchanged — it is the solution for threads without a
lane. `LaneTimerScope` is the solution for I/O-lane consumers.

The `timerfd_create` fd is owned by `TimerService` and submitted as a
persistent `IORING_OP_READ` on the lane's ring. The lane's CQE handler fires
expired entries from the timer heap.

**Portability:** conflux is currently Linux-targeted (`io_uring` throughout).
`timerfd_create` is Linux-specific. macOS/Windows ports are out of scope for
this phase; if a port is undertaken, `LaneTimerScope` will need a kqueue /
IOCP backend.

**Heap removal complexity:** `std::priority_queue` does not support O(log N)
random removal. Use lazy deletion: each entry carries a generation counter;
`~LaneTimerScope` increments the counter; the CQE handler skips entries whose
counter does not match. Removal is O(1) amortised (mark + dequeue-on-pop).
Specify this in the implementation; do not rely on linear scan.

**Tombstone compaction:** under deadline-heavy workloads where most timers
are cancelled before firing, the heap fills with tombstones and `pop` walks
through them, degrading to O(N). Compact when `tombstone_count > heap.size() / 2`
or every 1024 cancellations, whichever comes first: rebuild the heap by
filtering out generation-mismatched entries in one pass. Document the
threshold in code; revisit if traces show pathological patterns.

**Lane-thread-only — v1 constraint.** `LaneTimerScope` construction and
destruction must occur on the owning lane thread. Cross-thread cancellation
must be posted to the lane (e.g., via the lane's submission queue). This
keeps the heap and generation counters single-threaded — no atomics on the
hot path, no mutex on the heap.

**Enforcement:** debug builds assert lane-thread ownership. Release builds
log a one-time `WorkError` diagnostic to the configured carrier-error sink
on first violation (not free, but cheaper than a debug abort and visible
enough that the bug surfaces in production telemetry). A silent
release-build acceptance would let cross-thread construction corrupt the
timer heap without trace; the log gives a paper trail without breaking
release behaviour. Cross-thread timer construction is a common mistake.

**Sink contract — load-bearing:** the carrier-error sink used for this
diagnostic MUST be `noexcept` and **non-blocking** (lock-free ring buffer,
async post, etc.). A synchronous blocking log inside `~LaneTimerScope`
would stall the I/O lane and violate the O(1) destruction gate. Document
the sink concept in D1 with an explicit note: *"Sinks installed for
carrier diagnostics MUST satisfy noexcept + non-blocking; a blocking
sink will deadlock high-frequency carrier paths."* Provide a default
no-op sink for builds that have not configured one.

**`root::emit_carrier_diagnostic(const char *msg)` — primitive spec.** A
single noexcept free function in root that forwards to the registered
sink, used by R2/5c/Phase 8/Phase 8.5 defensive paths under
`CONFLUX_WORK_CHECKED_BUILD`:

```cpp
// root.cxx — install via root::set_carrier_diagnostic_sink(...) at startup;
// default sink is a noop. Sink type:
struct CarrierDiagnosticSink {
    void (*emit)(const char *msg) noexcept;  // function pointer; no allocation
};

void set_carrier_diagnostic_sink(CarrierDiagnosticSink) noexcept;
void emit_carrier_diagnostic(const char *msg) noexcept;

// Formatted variant — writes into a fixed thread_local stack buffer
// (e.g., 256 bytes) via vsnprintf, then forwards to the sink. No heap
// allocation, no exceptions; truncates with a "..." suffix on overflow.
// Used to log handle/coroutine/awaiter addresses in checked-build
// diagnostics (e.g., R7 in_flight race) so the offending site is
// recoverable from a crash log.
void emit_carrier_diagnostic_fmt(const char *fmt, ...) noexcept
    __attribute__((format(printf, 1, 2)));
```

Required by R2 awaiter dtors, DroppableSlot dtor, LaneTimerScope. Ship as
part of R2 PR (it is the only consumer until 8.5 lands). Diagnostic
content is a fixed C string — no allocation, sink-callable in noexcept
commit-thread context. Sink MUST itself be noexcept + non-blocking;
default sink is a noop. A blocking sink installed via
`set_carrier_diagnostic_sink` will deadlock the lane on `~LaneTimerScope`
and stall the commit thread on R2/5c/Phase-8 paths — the contract is
load-bearing across every consumer.

A future v2 may relax this with a per-lane mailbox + lock-free MPSC for
cross-thread cancellations, but only after a real consumer needs it.

**`timerfd` rearm on insert:** when a `LaneTimerScope` inserts an earlier
deadline than the current heap minimum, the lane must `timerfd_settime` to
the new minimum. Document this in the timer service implementation; missing
this rearm causes the new earlier deadline to wait until the previous-minimum
expiry.

### Priority Note

The http rebuild can ship phase 2 without this, using inline steady-clock
checks. This phase is here so subsequent consumers don't each reinvent the
same poll pattern. File after Phase 8 lands; do not block Phase 8 on it.

Exit criteria:
- Deadline-fire latency: median < 100 µs (regression gate, hard CI gate).
  p99 reported as a diagnostic, not a hard gate (CI noise is too high to
  reliably bound p99 under contended runs; use it to detect outliers, not
  to fail builds).
- `LaneTimerScope` destructor marks entry as cancelled without firing callback
  (test: destroy early, verify callback never called)
- N=10 000 `LaneTimerScope` destructions in sequence complete in < 1 ms
  (validates O(1) lazy-deletion; not O(N) scan) — measured on tmpfs build
  under normal load with the documented compaction threshold active
- Insertion-under-load: with N=1000 active timers, inserting a new earliest
  deadline (triggers `timerfd_settime` syscall, ~100-300 ns) completes in
  < 1 µs p99. Without this gate, deadline-heavy workloads can see
  pathological insertion latency.
- Insertion of an earlier deadline triggers `timerfd_settime` rearm; test
  inserts a 100ms deadline, then a 10ms deadline, asserts callback fires
  near 10ms (not 100ms)
- Cancel-earliest test: insert a 10ms timer, then a 100ms timer, then
  destroy the 10ms scope before it fires; assert the 100ms timer still
  fires near 100ms (not stuck on the cancelled-and-rearmed timerfd state)
- Lane-thread-ownership assertion fires in debug build when `LaneTimerScope`
  is constructed/destroyed off the owning lane
- No regression on existing `carrier_a/*` benchmarks
- `DeadlineScope` tests unchanged

---

## Phase 9: Opt-in Instrumentation + Budgets

### Goal

Add zero-overhead compile-time-gated timing instrumentation. Off by default.
Enabled via CMake option.

### Defer Decision

**Defer Phase 9 until after Phase 5/8 ship and at least one consumer reports
a real instrumentation need.** Premature instrumentation locks in fields,
hooks, and budget shapes that cannot be predicted without a real workload.
The design below is a sketch to be revisited; do not implement until a
consumer asks.

### Design Sketch (revisit when needed)

```cpp
// CMake option: CONFLUX_WORK_INSTRUMENTATION (default OFF)

// Composition, NOT inheritance. Public inheritance from Chain<T> risks
// slicing, surprises overload resolution, and was not part of Chain's
// original design. The macro toggle controls whether instrumentation
// fields exist inside Chain<T> itself, OR whether a separate TimedChain
// composes a Chain<T>; pick one at design time, do not mix.
//
// SCOPE: applies to NEW carrier surfaces introduced from Phase 9 onward.
// Existing surfaces that already use inheritance — notably
// carrier_deadline.cxx `class DeadlineScope : public Scope` — are
// grandfathered. DeadlineScope shipped under Phase 3 before this rule
// existed; reworking it as composition is out of scope. The rule prevents
// NEW slicing / overload-surprise bugs, not refactoring working code.

#if CONFLUX_WORK_INSTRUMENTATION
template<root::work_value T>
class TimedChain {
    Chain<T> chain_;                                  // composition
    std::chrono::steady_clock::time_point created_at_{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point admitted_at_{};
public:
    explicit TimedChain(Chain<T> &&c) noexcept : chain_{std::move(c)} {}
    [[nodiscard]] Chain<T>          &chain()           noexcept { return chain_; }
    [[nodiscard]] Chain<T> const    &chain() const     noexcept { return chain_; }
    [[nodiscard]] std::chrono::nanoseconds latency_to_admit() const noexcept;
};
// Note: instrumented admit must also return TimedChain<T>, NOT Chain<T>.
// The earlier sketch (`Chain<T> admit(..., AdmitOptions)`) was inconsistent;
// either return TimedChain or store the timing inside Chain behind the macro.
#else
template<root::work_value T>
using TimedChain = Chain<T>;  // free type pun when instrumentation off
#endif

struct AdmitOptions {
    std::chrono::nanoseconds budget{0};  // 0 = no budget
    BudgetViolation on_violation = BudgetViolation::log;
};

template<class Owner, root::work_value T>
TimedChain<T> admit(Owner&, root::PostedJoinHandle<T>&&, AdmitOptions opts = {});
```

Compile-time gated only. No runtime toggle. When `CONFLUX_WORK_INSTRUMENTATION`
is off, `TimedChain<T>` IS `Chain<T>` (alias) and all instrumentation
arguments compile away. `BudgetViolation::log` fires a user-supplied callback.
`BudgetViolation::assert` calls `std::terminate()` in debug builds.

Exit criteria:
- `CONFLUX_WORK_INSTRUMENTATION=OFF`: `sizeof(Chain<T>)` identical to Phase 4
- `CONFLUX_WORK_INSTRUMENTATION=OFF`: `TimedChain<T>` is `Chain<T>` (static_assert)
- `CONFLUX_WORK_INSTRUMENTATION=ON`: timing recorded correctly in tests
- Budget violation callback fires iff budget exceeded
- No regression on Phase 1 benchmark table when instrumentation is OFF
- No public inheritance from `Chain<T>` anywhere in the instrumentation surface

---

## Documentation Deliverables

Separate from code phases. Each item is a concrete document gap that causes
downstream consumers to read source rather than a contract reference. These
should land before or alongside the phases they cover.

### D1: Carrier API Reference (`docs/conflux-work-carrier-api.md`)

Equivalent to `conflux-work-root-api.md` but for the carrier layer. Covers:

- `Chain<T>`: what it is, what `release_outcome()` does, ownership rules
- `from_task` / `from_posted` / `from_operation`: preconditions, category
  semantics
- `map` / `then` / `when_all` / `when_all_fast_fail` / `race`: combinators,
  binding rules (drop vs preserve), error propagation
- `hop_to_posted` / `hop_to_operation` / `hop_to_task` / `unbind`: binding
  semantics, `CarrierKind` transitions
- `verify_hop`: when it throws, what `HopCapabilityError` is
- `Scope` / `DeadlineScope`: admit contract, liveness rules, `cancel`
  semantics, when NOT to use `DeadlineScope` (G4 note)
- `Scope::track` contention note (G5): single mutex; **supported design
  envelope n ≤ 32 tracked items per instance.** `Scope::cancel`
  (carrier_scope.cxx:77–102) actually swaps the registry vector under the
  mutex and iterates outside, so per-cancel mutex hold time is O(1) — the
  pathology is not lock-hold during cancel. The pathology is `track`/`untrack`
  serialisation: every `track` from a child carrier takes the same mutex,
  and high-fanout patterns (`when_all` over many siblings, HTTP request
  fan-out) serialise their setup phase through this single mutex. At n > 32
  setup-time contention dominates real workloads.

  **Enforcement:** debug-build assertion at the 32 boundary with a
  remediation message: `"Scope::track exceeded n=32; partition across multiple
  Scope instances or file a follow-up for concurrent registry"`. Release
  builds do NOT enforce — they remain semantically correct but may pay
  unbounded setup-time contention. This is honest "supported envelope" not
  a hard guarantee. Earlier draft called it a hard contract; that was
  overstatement (release builds don't check).

  Consumers needing higher fan-out (`when_all_fast_fail` over many siblings,
  `DeadlineScope` covering large fan-outs, HTTP request fan-out) MUST use
  multiple Scopes (one per partition) OR file a follow-up to lift the
  limit via a concurrent registry.
- `HopCapabilityError`, `CarrierKind`, `CapabilityId` sentinel rules
- Naming convention: `from_*(root_type) → Chain<T>` (construct carrier
  from root); `into_*(Chain) → root_type` (convert carrier back to root);
  `hop_to_*(cap, chain)` (re-bind and change kind). Document this table
  so future authors don't invent new patterns.

### D2: Cancel-Hook Safety Reference (G6)

Standalone section in the carrier API doc (or root API doc §Source Contract).
Answers what a noexcept one-shot cancel hook may safely do:

- **Posting to a lane is safe:** hook captures a lane handle or ring reference
  and posts a cleanup job (e.g., `IORING_OP_ASYNC_CANCEL` SQE submission);
  returns immediately. The cleanup job runs on the lane thread.
- **Closing an fd directly is risky:** depends on kernel version and in-flight
  SQE state; the recommended pattern is to cancel via SQE and let the CQE
  handler close the fd.
- **TLS shutdown is not safe inline:** `SSL_shutdown` can throw. Wrap in
  `try { … } catch (...) {}` inside the hook; the actual shutdown belongs in
  the cleanup job posted to the lane.
- **Re-entrancy:** hook fires synchronously on the `request_cancel()` caller's
  thread. If that thread is the lane thread (e.g., a CQE handler calling
  cancel), the posted cleanup job must be deferred (use a non-reentrant
  submission path or check `is_ring_thread()`).

### D3: Source Commit/Cancel-Hook Race Guarantee (G9)

Addition to root API doc §Source Contract. Documents:

- `install_cancel_hook` followed by `commit_*` on a different thread is safe.
  Exactly one of `commit_success`, `commit_failure`, `commit_cancelled` wins as
  the terminal commit — the others are no-ops.
- The cancel hook is **advisory and independent of terminal commit.** A call to
  `request_cancel()` fires the hook at most once (via `invoke_requested_hook_if_needed`),
  but does NOT prevent a subsequent `commit_success` from winning the terminal
  CAS. Both can happen: hook fires (cancel requested) and terminal is committed
  as success (because the work completed before the cancel took effect). This
  is by design — `request_cancel` is a hint, not a preemption. Callers must
  not assume that a fired cancel hook means the source cannot commit success.
- Hook installed after terminal commit fires immediately on the calling thread
  (existing guarantee; should be more prominent in the docs).
- **R2 abandon-clears-on_ready contract (REQUIRED for Phase 8 drain
  correctness).** When the producer side abandons the control block without
  calling `commit_*` (e.g., `~TaskSource` without commit, panic-driven
  drop), the abandon path MUST either (a) clear the installed
  `on_ready_fn_` so the lambda's owned join handle drops and the cycle
  breaks, OR (b) transition the control block to a terminal cancelled
  state and fire the callback as part of abandon. Without this, a dropped
  unready `DroppableSlot` whose producer is later abandoned without
  commit leaks the control block forever (drain lambda owns the handle,
  handle owns the control block, no commit ever clears the lambda).
  Test: drop unready slot → abandon producer (no commit) → assert control
  block reclaimed via LeakSanitizer.

### D4: `abandon_to` Failure Mode Pattern

Addition to root API doc §Abandonment APIs. Provides a sanctioned pattern
for abandon sinks that want to log without risking a throw:

```cpp
auto sink = root::make_abandon_sink([](root::AbandonedHandle handle) noexcept {
    try {
        log_orphaned(handle.description());
    } catch (...) {
        // logging threw; no further action — terminate is the only alternative
        // and is worse than silence
    }
});
```

### D5: `MoveOnlyFunction` Inline Buffer Size (Doc-6)

Addition to root API doc §Implementation Notes. Documents:

- The `MoveOnlyFunction<Sig>` inline buffer size (currently implementation-
  defined by the callable_erasure implementation). Record the actual size
  after R2's control block layout audit.
- Cancel hooks and `on_ready` callbacks that capture `(fd, ring_handle, …)`
  may exceed the inline buffer and heap-allocate. Callers who want to avoid
  this allocation on the hot path should measure with the `callable_erasure_*`
  benchmarks and restructure captures to fit the buffer if needed.
- No public size-hint knob is exposed today. If the audit shows a consistent
  overflow pattern across consumers, a `MoveOnlyFunction<Sig, InlineCap>`
  template parameter may be added as a follow-up.

---

## Phase 10: Re-evaluation Pass

These items were deferred pending real measurement data or post-implementation
experience.

### 10a: Stop-State Allocation — Closed

Already implemented. `ControlBlockModel` uses
`std::conditional_t<EnableCancellation, std::stop_source, std::monostate>`.
No action required.

### 10b: Debug Terminate Alternatives

Current: live-chain destructor calls `std::terminate()`.

Candidate: `CONFLUX_WORK_CHECKED_BUILD` CMake option installs a custom terminate
handler that emits diagnostic (chain ID, allocation site) before aborting. Not
a no-terminate option — the contract stays hard. Only diagnostic quality changes.

Gate: post-implementation experience. If `guard_abandon(...)` consistently catches
live-destroy bugs in review, close. If integration testing still produces
hard-to-diagnose terminates, implement the diagnostic handler.

Exit criteria:
- Decision: "close" (evidence: N months, M live-chain bugs caught via review
  rather than terminate) OR "implement" (evidence: N hard-to-diagnose
  terminate calls in integration testing over the same period)
- If implement: custom handler fires and emits diagnostic before abort; verified
  by a test that constructs a live chain without consuming it and checks the
  diagnostic output

### 10c: `joinable` Helper — Promoted To R4

Moved to Root Layer Changes (R4). Closed.

### 10d: Callable-Disambiguation Helpers

Current: root admission uses `MoveOnlyFunction` with exact-one-form rule.
Diagnostics fire when multiple overloads match.

Candidate: carrier-layer `as_task_fn(...)` / `as_posted_fn(...)` wrappers.

Gate: collect real-world call-site data.

Exit criteria:
- Decision: "implement" iff >10% of root admission calls in downstream code
  require manual disambiguation (measured by counting `as_task_fn`-equivalent
  wrappers in downstream code after 3 months of carrier use)
- Decision: "close" otherwise, with recorded measurement

### 10e: `Outcome<T>::visit` + `value()` — Promoted To R3

With root now changeable, both land directly in root as `value()` and `match()`.
See R3. Closed.

---

## Priority Order

Reordered per cross-model review consensus: cancel-hook docs before coroutine
phases (mental model first), `EagerChain` after `5c` (sync-only contract is
load-bearing and easier to reason about once the async path exists), Phase 8
before `5b` (drain mechanism is the harder design problem).

| Work item | Value | Effort | Priority |
|---|---|---|---|
| R3 Outcome value()/match() | High — daily-use ergonomics; http rebuild G8 | Low | 1 |
| R1 JoinContextError non-final | High — unblocks Phase 6 + correct hierarchy | Trivial | 2 |
| R4 joinable() | Low — thin wrapper | Trivial | 3 |
| R6 BasicJoinHandle operator bool | Trivial — required by 5c defensive abandon | Trivial | 3.5 |
| R2 try_set_on_ready + state machine + R7 clear_on_ready + set_on_ready_or_run | High — unblocks 5c, Phase 7, Phase 8; lands as one PR (R7 + helper inseparable from R2 protocol) | Medium-High | 4 |
| D2 + D3 cancel-hook + commit-race docs | High — **HARD merge prerequisite for 5c and Phase 8**; consumers will misuse drain hooks and cancel hooks without these | Low | 5 |
| 5a Chain<T> co_await | High — unlocks coroutine style | Low | 6 |
| 5c Async root suspension | High — true async coroutines; http rebuild G1 | Medium | 7 |
| 6 Carrier completions + into_ready_task() | Medium — ergonomics polish; http rebuild G3 | Low | 8 |
| 8 DroppableSlot (with drain hook) | High if http rebuild ships first (G7); else Medium | Medium | 9 |
| 5b EagerChain<T> | Medium — sync-only coroutine sugar | Low-Medium | 10 |
| 8.5 Per-lane timer service | Medium — required for high-concurrency I/O (G4) | Medium | 11 |
| D1 + D4 + D5 remaining docs | High — prerequisite for safe consumer code | Low | 12 |
| 7 std::execution adapters | Medium — ecosystem interop | High | 13 |
| 9 Instrumentation | Low-Medium — defer until consumer asks | Medium | 14 |
| 10b/10d Re-evaluation | Contingent on data | Varies | 15 |
| 11 G2b: FileReader/TLS → Operation<T> | High long-term; G2c unblocks short-term | Large | 16 (owner: TBD) |

**Phase 8 priority escalation:** if any public-API-fluent subsystem (e.g.,
the http client rebuild) ships before Phase 8 lands, move Phase 8 to
priority 5 (immediately after R2). The destructor-terminate default is a
sharp edge in a builder/co_await-style API that cannot be deferred past the
first public consumer.

## Invariants Across All Phases

- `root::Task<T>`, `root::Posted<T>`, `root::Operation<T>` are not
  `std::execution` senders (until Phase 7 adapter wrapper; never on root types
  themselves — the adapter is a separate type).
- `Chain<T>` remains the universal carrier output type. No new carrier output
  types unless a phase explicitly adds them.
- Every hop, verify, and admit operation remains explicit. No implicit domain
  inference in any phase.
- The Phase 1 benchmark table is the perf gate. No phase may regress it beyond
  the documented guard-band without an explicit decision record.
- Root semantic contracts (liveness, capability enforcement, cancellation,
  allocation) are preserved across all root surface changes.
