# `conflux.work` Phase 4 Design: Affinity Hop Surface

Status: reviewed — ready for implementation

Source alignment:

- `conflux-work-api-redesign-deferred-plan.md` Phase 4
- `conflux-work-api-redesign-decisions.md` §Affinity: Partial Decision
- `conflux-work-api-redesign-proposal-v10.md` §What Is Deferred, §Affinity

## Problem Statement

Phase 1 introduced `bridge_to_posted` and `bridge_to_operation` as placeholder
cross-category transition functions. These have two deficiencies:

1. **No capability binding:** both functions accept a capability reference but
   ignore it. The returned `Chain<T>` carries no record of which capability it
   was hopped to, so nothing prevents using the wrong owner/driver later.

2. **No name finalization:** "bridge" is an internal Phase 1 placeholder.
   V10 named the intent `hop_to(target, source)` / `resume_on(target)`.
   Phase 4 must finalize the spelling.

The decisions doc also requires a **migratable domain policy** contract:
any migratable chain must specify a single allowed resume domain, not "hop
anywhere." Phase 4 implements this at the carrier level.

## Locked Constraints (Root Layer)

Root contracts are unchanged. Notably:

- `join(owner, posted)` already throws `JoinContextError` on capability
  mismatch — root is the final enforcement point.
- `CapabilityId {void* address, void* type_tag}` is exported and comparable.
- `progress_capability` concept requires `capability_id(cap)` → `CapabilityId`.

Phase 4 adds a carrier-layer check that fires **before** a chain reaches root,
giving earlier and more readable diagnostics.

## Design Decisions

### D1: Capability Binding Via Sentinel Field On `Chain<T>`

`Chain<T>` gains one field:

```cpp
root::CapabilityId bound_cap_{};
```

`{nullptr, nullptr}` is the "unbound" sentinel. `progress_capability` cannot
legally produce a null-address `CapabilityId`, so the sentinel is unambiguous.
An unbound chain has no affinity constraint: it passes any carrier-level
capability check. All existing `Chain<T>` construction paths remain valid —
the field value-initializes to the sentinel.

Rationale: a separate `BoundChain<T>` wrapper would proliferate types and
require callers to switch between `Chain<T>` and `BoundChain<T>` at every hop.
The sentinel field is invisible to code that does not call `hop_to_*` or
`verify_hop`. Sentinel storage is 16 bytes flat; `std::optional<CapabilityId>`
would be 24 bytes (16 + 1 bool + 7 pad alignment) — 8 bytes/chain saved.

### D2: `hop_to_posted` / `hop_to_operation` As Finalized Hop Names

Replace `bridge_to_*` with `hop_to_posted` and `hop_to_operation`. Same move
semantics, but these now:

- set `CarrierKind` to the matching category
- store `capability_id(cap)` in the chain's `bound_cap_`

```cpp
template<root::work_value T, root::progress_capability Owner>
[[nodiscard]] Chain<T> hop_to_posted(Owner &owner, Chain<T> &&chain) noexcept;

template<root::work_value T, root::progress_capability Driver>
[[nodiscard]] Chain<T> hop_to_operation(Driver &driver, Chain<T> &&chain) noexcept;
```

`bridge_to_*` are removed (not deprecated with alias — they were never stable
API, only Phase 1 scaffolding). Tests and benchmarks that call `bridge_to_*`
are updated to `hop_to_*`.

### D3: `verify_hop` For Explicit Capability Checks

A free function that checks a chain's bound capability against a given
capability:

```cpp
template<root::progress_capability Cap, root::work_value T>
void verify_hop(Cap const &cap, Chain<T> const &chain);
```

Behavior:
- If `chain.bound_capability().address` is null → no-op (unbound chains pass).
- If `chain.bound_capability()` equals `capability_id(cap)` → no-op.
- Otherwise → throws `HopCapabilityError`.

`HopCapabilityError` is a new exception type extending `root::JoinContextError`.

### D4: Accessor On `Chain<T>`

```cpp
[[nodiscard]] root::CapabilityId bound_capability() const noexcept;
```

Returns the bound `CapabilityId`. A null `.address` means unbound. Read-only;
binding can only be set via `hop_to_*` or direct construction.

### D5: Migratable Domain Policy Contract (Documentation + Type)

"Migratable" means a chain that has been explicitly hopped to a single
allowed resume domain. The policy:

- A chain without a bound capability is **free** (no domain constraint; use
  anywhere).
- A chain with a bound capability is **migratable-to-one**: only the named
  domain is allowed for subsequent capability-gated operations.
- Rebinding is allowed via `hop_to_*` (replaces the previous binding).
- The constraint is advisory in the carrier layer (checked only by
  `verify_hop`); root `join(cap, ...)` provides the hard enforcement.

There is no compile-time multi-domain allowlist in Phase 4. The deferred
plan notes "not hop anywhere" — one bound target per chain satisfies this.
Multi-domain policies remain deferred.

### D6: Removed API

- `model_a::bridge_to_posted` — removed; callers use `hop_to_posted`
- `model_a::bridge_to_operation` — removed; callers use `hop_to_operation`

Model B equivalents if present are updated in parallel.

## New Exception Type

```cpp
class HopCapabilityError final : public root::WorkError {
public:
    using WorkError::WorkError;
};
```

`HopCapabilityError` is a context/misuse error (wrong hop target before admission), not a terminal async outcome. It derives from `root::WorkError` rather than `root::JoinContextError` because `JoinContextError` is declared `final` in root and cannot be subclassed. Both share `WorkError` as their common base, so `catch (root::WorkError &)` catches carrier hop misuse and root join misuse uniformly.

Lives in `carrier_model_a.cxx` (same module as `Chain<T>`).

## `Chain<T>` Changes

Full updated class shape (additions marked):

```cpp
template<root::work_value T>
class Chain {
    root::Outcome<T> outcome_;
    CarrierKind kind_ = CarrierKind::task;
    root::CapabilityId bound_cap_{};                          // NEW; {} = unbound sentinel

public:
    Chain() = delete;

    Chain(root::Outcome<T> outcome, CarrierKind kind) noexcept
        : outcome_{std::move(outcome)}, kind_{kind} {}

    Chain(root::Outcome<T> outcome, CarrierKind kind,         // NEW
          root::CapabilityId cap) noexcept
        : outcome_{std::move(outcome)}, kind_{kind}
        , bound_cap_{cap} {}

    Chain(Chain &&) noexcept = default;
    Chain &operator=(Chain &&) noexcept = default;
    Chain(Chain const &) = delete;
    Chain &operator=(Chain const &) = delete;

    [[nodiscard]] CarrierKind kind() const noexcept;
    [[nodiscard]] root::CapabilityId bound_capability() const noexcept; // NEW; null = unbound

    [[nodiscard]] root::Outcome<T> release_outcome() && noexcept;
};
```

## `hop_to_*` Implementations

```cpp
template<root::work_value T, root::progress_capability Owner>
[[nodiscard]] Chain<T> hop_to_posted(
    Owner &owner, Chain<T> &&chain) noexcept {
    return Chain<T>{
        std::move(chain).release_outcome(),
        CarrierKind::posted,
        root::capability_id(owner)};
}

template<root::work_value T, root::progress_capability Driver>
[[nodiscard]] Chain<T> hop_to_operation(
    Driver &driver, Chain<T> &&chain) noexcept {
    return Chain<T>{
        std::move(chain).release_outcome(),
        CarrierKind::operation,
        root::capability_id(driver)};
}
```

## `verify_hop` Implementation

```cpp
template<root::progress_capability Cap, root::work_value T>
void verify_hop(Cap const &cap, Chain<T> const &chain) {
    auto const bound = chain.bound_capability();
    if (bound.address && bound != root::capability_id(cap)) {
        throw HopCapabilityError{"carrier: hop capability mismatch"};
    }
}
```

## Correctness Matrix

- Unbound chain: `verify_hop` with any capability → no throw
- Bound chain with matching capability: `verify_hop` → no throw
- Bound chain with wrong capability: `verify_hop` → throws `HopCapabilityError`
- `hop_to_posted` sets kind to `posted` and records capability
- `hop_to_operation` sets kind to `operation` and records capability
- Rebind via second `hop_to_*`: new binding replaces old
- `map` / `then`: output chain is unbound (binding dropped; only `hop_to_*` establishes binding)
- `when_all` / `when_all_fast_fail`: aggregate chain is unbound
- `race`: winner's `bound_cap_` is preserved (winning chain is moved into result)

## Interaction With Existing APIs

### `map` / `then`

Both call `release_outcome()` on input and construct a new `Chain<U>`. The
new chain loses the bound capability (no binding is forwarded). This is
correct: a `map` transform re-emits a result in the same domain implicitly,
but the phase-4 policy says only `hop_to_*` establishes binding.

If a caller wants the mapped result to remain bound, they call `hop_to_*`
after `map`. This keeps binding explicit.

### `when_all` / `when_all_fast_fail`

Both construct `Chain<tuple<A,B>>` from scratch. No binding propagated — the
combined chain is unbound. Callers who want to bind the aggregate result use
`hop_to_*` afterwards.

### `race`

Returns the winner's chain. The winner's `bound_cap_` is preserved since the
winning chain is moved into the result. This is the natural behavior: if the
winner was bound, the result remains bound.

### `Scope::admit`

Already calls `root::join(owner/driver, jh)` which does the hard capability
check at root level. No carrier-layer `verify_hop` needed in `admit` — root
is the enforcer there.

## Files Changed

| File | Change |
|------|--------|
| `src/work/carrier_model_a.cxx` | Add `bound_cap_` to `Chain<T>`; add `hop_to_*`; remove `bridge_to_*`; add `HopCapabilityError`; add `verify_hop` |
| `tests/work_carrier_test.cxx` | Update `bridge_to_*` calls → `hop_to_*`; extend with capability tests |
| `tests/work_carrier_phase4_test.cxx` | New: Phase 4 correctness matrix tests |
| `tests/CMakeLists.txt` | Register new test target |
| `benchmarks/work_bench.cxx` | Update `bridge_to_*` → `hop_to_*`; add hop benchmark cases |
| `docs/implementation-design/conflux-work-phase4-hop-surface-design.md` | This doc |

Model B (`carrier_model_b.cxx`): updated in parallel if `bridge_to_*`
equivalents exist there.

## Closed Decisions (Post-Review)

1. **`map`/`then` binding:** **Drop.** Only `hop_to_*` establishes binding;
   transforms do not propagate it. Callers who want the mapped result bound
   call `hop_to_*` after `map`. This keeps affinity visible at every hop site.
   Performance: equivalent (drop = zero-init, propagate = copy 2 pointers).
   Decision by ergonomics/explicitness.

2. **`HopCapabilityError` location:** `carrier_model_a.cxx`. Co-located with
   `Chain<T>`. Move to a shared module if Phase 5+ adds more error types.

3. **`bridge_to_*` removal:** **Remove** (no deprecation alias). They were
   never stable API — Phase 1 scaffolding only.

4. **`when_all` binding:** **Always unbound.** Aggregate has no inherent
   single domain. Callers re-bind if needed. No branch or comparison overhead.

5. **`race` binding:** **Winner's binding preserved.** Winning chain is moved
   into the result naturally — zero overhead. Non-determinism when both inputs
   have different bindings is a known tradeoff; callers wanting deterministic
   binding call `hop_to_*` on the aggregate.

6. **`bound_cap_` storage:** **Sentinel** (`root::CapabilityId{}` = null
   address = unbound) rather than `std::optional`. Saves 8 bytes/chain
   (16 bytes flat vs 24 with optional's discriminant + alignment padding).
   `root::JoinContextError` is `final`; `HopCapabilityError` derives from
   `root::WorkError` (same parent) — `catch (root::WorkError &)` covers both.

7. **`Scope::admit` auto-bind:** Deferred. Changes Phase 2 return types for
   a convenience gain; no performance difference. Revisit in Phase 5 if
   callers consistently need to rebind after `admit`.

8. **`CarrierKind::task` + bound cap:** Constructing `Chain` with `kind=task`
   and a non-null `bound_cap_` via the two-arg constructor leaves `bound_cap_`
   at sentinel. The three-arg constructor (`outcome`, `kind`, `cap`) is for
   `hop_to_*` use only; task chains are always created via the two-arg path.
   No guard needed — this is a construction-path convention, not an invariant
   root can violate.

9. **Lifetime of `bound_cap_`:** The binding is valid only while the bound
   capability object is alive. It is a non-owning identity snapshot — same
   semantics as a raw pointer to the capability. If the capability dies and
   another is constructed at the same address, a stale binding would compare
   equal. Callers must ensure the bound capability outlives any `verify_hop`
   or `admit` call that uses this chain.
