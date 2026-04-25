# `conflux.work` Deferred Decisions Plan (Post-V10)

Status: proposed execution plan derived from deferred items in
`conflux-work-api-redesign-decisions.md` and
`conflux-work-api-redesign-proposal-v10.md`.

## Scope

This plan covers only decisions explicitly deferred by V10 and the working
decision log. It does not reopen locked root contracts.

## Deferred Inventory

### A. Carrier/Composition Layer

Deferred items:

- final coroutine/combinator carrier shape
- aggregate/combinator naming and callback placement
- helper abstractions over `join(...)` / `value(...)`

Primary source sections:

- decisions: `Coroutine / Composition Model`
- proposal-v10: `What Is Not Locked Here`, `Aggregates And Combinators`

### B. Sender/Receiver Adapter Surface

Deferred items:

- `std::execution` adapters for root objects
- final adapter API surface and ownership semantics

Primary source sections:

- proposal-v10: `Relationship To std::execution`, `Explicitly Deferred`

### C. Affinity/Hop Surface

Deferred items:

- final hop primitive spelling and shape
- migratable-domain policy

Primary source sections:

- decisions: `Affinity: Partial Decision, Final Shape Deferred`
- proposal-v10: `Affinity / What Is Deferred`

### D. Cancellation Sources Beyond Manual Request

Deferred items:

- deadline-triggered cancellation
- parent/scope-triggered cancellation

Primary source sections:

- proposal-v10: `Explicitly Deferred Cancellation Sources`

### E. Structured Concurrency And Time APIs

Deferred items:

- structured-concurrency scopes
- timeout/deadline primitives

Primary source sections:

- proposal-v10: `What Is Not Locked Here`, `Explicitly Deferred`

### F. Droppable/Coalescing Streams

Deferred items:

- droppable primitives
- latest-wins/coalescing primitives

Primary source sections:

- decisions: `Droppable / Coalescing Work`
- proposal-v10: `Explicitly Deferred`

### G. Performance Instrumentation And Budgets

Deferred items:

- opt-in timing instrumentation
- per-admission time budgets

Primary source sections:

- decisions: `Deferred Instrumentation Idea`
- proposal-v10: `Explicitly Deferred`

### H. Post-Implementation Evaluation Items

Deferred/revisit items:

- lazy/optional stop-state allocation
- debug-only alternatives to hard destructor `terminate`
- callable disambiguation helpers
- root-level `joinable` helpers

Rejected in V10 (no action unless policy changes):

- root-level heavy-payload offload traits
- relaxing strict `Outcome<T>::visit(...)` return-type rule

Primary source section:

- proposal-v10: `Post-Implementation Evaluation`

## Prioritization Rules

- Preserve root-layer contracts as-is unless explicitly re-approved.
- Land features in carrier/adapters first, then optional DX/perf extras.
- Require benchmark + test evidence before changing memory/layout-sensitive
  behavior.
- Prefer additive new layers over root-surface churn.

## Execution Plan

## Phase 1: Carrier Layer Decision And MVP

Goal:

- choose and ship one concrete carrier/composition mechanism above root types.

Deliverables:

- carrier RFC selecting one model from:
  - generic carrier above root categories, or
  - root-coroutine types + explicit bridges
- MVP APIs for chaining and transformation across category boundaries
- deterministic placement/affinity rules for combinator callbacks
- benchmark suite comparing candidate approaches on:
  - owner hot path
  - mixed category chain
  - cancellation overhead

Exit criteria:

- one design selected with benchmark evidence
- integration tests for mixed-chain composition
- no regressions in existing root microbench thresholds beyond agreed guardband

## Phase 2: Aggregates And Structured Concurrency Baseline

Goal:

- provide first stable aggregate and scope model in carrier layer.

Deliverables:

- `when_all` wait-all API (locked semantic)
- explicit fail-fast variant with best-effort sibling cancellation contract
- `race(...)` API with loser ownership/abandon semantics
- scope primitive with parent-triggered cancellation

Exit criteria:

- aggregate ownership and cancellation contracts documented
- stress tests for scope shutdown ordering and race loser handling
- no hidden thread/helper runtime requirement

## Phase 3: Deadlines, Timeouts, And Cancellation Sources

Goal:

- add time-based cancellation and scoped cancellation sources cleanly.

Deliverables:

- deadline and timeout wrappers in carrier layer
- cancellation reason mapping policy for deadline/scope cancellations
- adapter hooks from timer subsystem to control `request_cancel()` path

Exit criteria:

- explicit API contract for deadline expiration semantics
- tests for before-start vs running cancellation behavior by category

## Phase 4: Affinity Hop Surface

Goal:

- expose explicit hops without weakening capability rules.

Deliverables:

- finalized spelling and semantics for hop operations (placeholder names in V10)
- migratable domain policy contract (allowed resume domains only)
- capability-preservation checks across hops

Exit criteria:

- proven no implicit capability-context invalidation
- owner/driver mismatch tests continue to fail safely

## Phase 5: `std::execution` Adapter Layer

Goal:

- add sender/receiver interop as adapters, not root mutation.

Deliverables:

- sender wrappers/adapters for root results and handles
- `connect/start` bridge with explicit ownership and cancellation notes
- adapter-level diagnostics and examples

Exit criteria:

- interop tests with representative receiver chains
- root API remains non-sender by contract

## Phase 6: Droppable/Coalescing Stream Primitives

Goal:

- add stream-focused late-data policies separate from root result model.

Deliverables:

- droppable stream primitive(s)
- coalescing/latest-wins primitive(s)
- overflow/drop metrics hooks

Exit criteria:

- semantics clearly distinct from root `Task/Posted/Operation` liveness model
- correctness/perf tests under burst load

## Phase 7: Optional Instrumentation And Budget Enforcement

Goal:

- add opt-in diagnostics without making them semantic requirements.

Deliverables:

- compile-time-gated timing instrumentation
- optional per-admission budget fields in higher-layer APIs
- release-mode opt-in configuration knobs

Exit criteria:

- zero overhead when disabled (verified by benchmark)
- actionable over-budget diagnostics when enabled

## Phase 8: Re-Evaluation Pass On Deferred Perf/DX Items

Goal:

- revisit items deferred pending measurement.

Deliverables:

- profile-driven report for stop-state allocation strategy
- feasibility report for debug-only destructor alternatives
- recommendation on callable-disambiguation helper need
- recommendation on generic `joinable` helper placement

Exit criteria:

- accepted/rejected status for each deferred item recorded with data
- no change merged without explicit contract update note

## Proposed Immediate Next Sprint

- Execute Phase 1 discovery and decision only:
  - implement two minimal carrier prototypes behind compile-time flags
  - run benchmark matrix and choose one
  - publish decision doc + migration notes for next implementation phase
