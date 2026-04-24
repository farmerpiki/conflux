# `conflux.work` API Redesign Decisions

Status: working decision log

This document captures the decisions agreed before writing the replacement
plan. It is intentionally narrower than the replacement draft and focuses on
what is locked, what is deferred, and what is intentionally omitted for now.

## Locked Decisions

### 1. C++26 Target And Compatibility Stance

The redesign targets C++26 directly.

Locked rules:

- the replacement surface is C++26-only
- there is no requirement to preserve source, ABI, or naming compatibility
  with the current `conflux.work` API
- the redesign may adopt standard C++26 vocabulary types and concepts
  directly when that improves the public contract instead of keeping legacy
  compatibility shims

Rationale:

- this redesign is replacing the current surface, not wrapping it
- C++26 gives the library a better vocabulary for callable constraints,
  cancellation interop, allocators, and future execution adapters
- carrying legacy compatibility constraints into the redesign would force
  weaker semantics and more transitional surface area than the new model wants

### 2. Root Execution Categories

The redesign will use three root execution categories, not two:

- autonomous scheduled work
- explicitly posted owner work
- driver / CQE / state-machine driven operations

Rationale:

- `WorkPool` jobs, posted owner work, and driver-owned operations have
  materially different driving, affinity, cancellation, and observation
  semantics.
- A two-category split is still too coarse for conflux's real execution
  models.

Exact public names are not locked yet. The semantic split is locked.

### 3. Coroutine / Composition Model

The exact mechanism is deferred pending implementation experiments and
benchmarks, but the requirement is locked.

Locked requirement:

- mixed async chains must remain expressible without hidden helper runtime
- hot owner paths must not be forced into a hop-friendly model

Open design space:

- a generic composition / coroutine carrier above the root categories
- only root-category coroutine types plus explicit bridges

The replacement plan must treat this as an explicit benchmark-driven choice,
not as a settled decision.

### 4. Cancellation And Aggregation Contract

The generic cancellation contract is best-effort, not guaranteed prompt stop.

Locked rules:

- `request_cancel()` is a request, not a promise of immediate completion
- generic code must not depend on prompt sibling cancellation
- `when_all(...)` defaults to wait-all semantics
- separate fail-fast forms may exist later, but they may only request sibling
  cancellation and must document that unfinished work can continue briefly or
  indefinitely

Rationale:

- user code can ignore cancellation
- foreign blocking calls may not expose an interrupt path usable by conflux
- kernel / remote / protocol cancellation is often advisory or racey
- owner-driven cancellation still depends on continued owner progress

### 5. Droppable / Coalescing Work

Ordinary async result objects remain non-droppable by default.

Real-time "late data is useless" use cases are acknowledged, but they are
treated as a separate stream / delivery-policy problem, not as a reason to
weaken the core result-lifetime model.

Deferred TODO:

- separate droppable stream primitives
- separate coalescing / latest-wins primitives

These are intentionally omitted from the current redesign.

## Affinity: Partial Decision, Final Shape Deferred

### Locked Direction

Affinity is part of the public semantic contract.

The working direction is:

- critical ring / owner work keeps hard affinity
- longer-running coroutine work may use an explicit migratable policy
- any migratable policy must define an allowed resume domain, not "hop
  anywhere"

Affinity semantics must be explicit and stable. They must not depend on
optimizer behavior or inferred coroutine-frame properties.

### Performance Hints And Diagnostics

Performance intent may be annotated separately from affinity semantics.

Possible examples:

- critical / latency-sensitive annotation
- heapless-expected annotation
- explicit per-spawn or per-admission time budget

If exposed, these are advisory / validation-oriented features. They do not
change affinity semantics by themselves.

### Deferred Instrumentation Idea

The redesign should keep open the option to add opt-in timing enforcement for
coroutine work later.

Deferred idea:

- optional time budget parameter on coroutine spawn / admission
- optional compile-time-gated timing instrumentation
- opt-in availability in release builds for users who want it
- customizable time thresholds

The main intended use is to catch "critical" coroutine work that exceeds its
budget on latency-sensitive threads.

Coroutine-frame allocation status may still be useful as a weak hint, but it is
not strong enough to serve as the primary policy decision or enforcement
signal.

The exact instrumentation model is deferred until benchmarking and
implementation experiments.

## Implementation Follow-Ups

- callable-erasure performance:
  keep the root layer on a move-only callable path (no `std::function`) and
  benchmark a dedicated low-overhead implementation against
  `std::move_only_function` as toolchain support stabilizes. Preserve
  move-only semantics and avoid adding wrapper overload compatibility layers.

## Migration Progress

- compatibility bridge APIs in `conflux.work` (`from_root_*` / `to_root_*`)
  have been removed; the module now exposes only native flow/task APIs plus
  direct `conflux.work.root` usage.
- root admission options now include `enable_cancellation` (default `true`) so
  task/posted/operation sources can opt into an inert stop-token path.
- control-source factories now also accept the corresponding options
  (`SubmitOptions` / `PostOptions` / `OperationOptions`) so explicit-control
  admission can use the same inert stop-token fast path.
- root no-cancellation tests now cover task/posted/operation admission and
  task/posted/operation control-source paths for both inert stop-token
  observation and cancel-hook execution.
- bridge-focused tests were removed from `tests/work_test.cxx`; root-contract
  coverage lives in `tests/work_root_test.cxx`.
- root-path microbench snapshot (`release-clang-libcxx`,
  `conflux_work_benchmarks --filter root/ --iterations 500000`) currently
  reports:
  - `root/task_join_success`: ~81.5 ns/iter
  - `root/posted_join_success`: ~80.8 ns/iter
  - `root/operation_join_success`: ~79.7 ns/iter
  - `root/cancel_hook_enabled`: ~132.1 ns/iter
  - `root/cancel_hook_disabled`: ~113.0 ns/iter
  - `root/control_cancel_hook_enabled`: ~93.9 ns/iter
  - `root/control_cancel_hook_disabled`: ~76.1 ns/iter
  - `root/abandon_sink_cancelled`: ~91.8 ns/iter
  - `root/callable_erasure_custom`: ~1.6 ns/iter
- callable-erasure comparison snapshot (`release-clang-libcxx`,
  `conflux_work_benchmarks --filter callable_erasure --iterations 2000000`)
  currently reports:
  - `root/callable_erasure_custom`: ~1.7 ns/iter
  - `root/callable_erasure_custom_inline24`: ~1.7 ns/iter
  - `root/callable_erasure_custom_inline32`: ~1.6 ns/iter
  - `root/callable_erasure_std_function`: ~1.6 ns/iter
  - `root/callable_erasure_custom_capture24`: ~1.8 ns/iter
  - `root/callable_erasure_std_function_capture24`: ~1.7 ns/iter
  these micro-results are noisy and currently indicate near-parity for small
  callables on this libc++ toolchain rather than a decisive winner. The
  inline-size A/B does not currently justify shrinking the root default from
  32 bytes.
- callable-erasure benchmark phase is now wired into
  `benchmarks/work_bench.cxx` as `root/callable_erasure_custom`; the
  `std::move_only_function` comparison case remains compile-gated until the
  active standard library exposes it.
- benchmark dispatch no longer uses `std::function`; `BenchFn` now uses the
  same move-only callable erasure family to avoid adding wrapper overhead to
  tight root-path microbench loops.
- cancel-hook microbench snapshot (`release-clang-libcxx`,
  `conflux_work_benchmarks --filter cancel_hook --iterations 500000`)
  currently reports:
  - `root/cancel_hook_enabled`: ~129.8 ns/iter
  - `root/cancel_hook_disabled`: ~110.8 ns/iter
  - `root/posted_cancel_hook_enabled`: ~132.5 ns/iter
  - `root/posted_cancel_hook_disabled`: ~114.3 ns/iter
  - `root/operation_cancel_hook_enabled`: ~135.4 ns/iter
  - `root/operation_cancel_hook_disabled`: ~115.0 ns/iter
  - `root/control_cancel_hook_enabled`: ~97.9 ns/iter
  - `root/control_cancel_hook_disabled`: ~80.6 ns/iter
- control-source cancel-hook microbench snapshot (`release-clang-libcxx`,
  `conflux_work_benchmarks --filter control_cancel_hook --iterations 500000`)
  currently reports:
  - `root/control_cancel_hook_enabled`: ~98.1 ns/iter
  - `root/control_cancel_hook_disabled`: ~74.9 ns/iter
  - `root/posted_control_cancel_hook_enabled`: ~94.8 ns/iter
  - `root/posted_control_cancel_hook_disabled`: ~74.4 ns/iter
  - `root/operation_control_cancel_hook_enabled`: ~98.0 ns/iter
  - `root/operation_control_cancel_hook_disabled`: ~75.1 ns/iter
- admission microbench snapshot (`release-clang-libcxx`,
  `conflux_work_benchmarks --filter admission --iterations 500000`)
  currently reports:
  - `root/task_admission_enabled`: ~105.9 ns/iter
  - `root/task_admission_disabled`: ~90.6 ns/iter
  - `root/posted_admission_enabled`: ~103.4 ns/iter
  - `root/posted_admission_disabled`: ~89.5 ns/iter
  - `root/operation_admission_enabled`: ~104.8 ns/iter
  - `root/operation_admission_disabled`: ~94.0 ns/iter
  admission benchmark cases now explicitly `abandon_to(...)` created live
  results so they honor the root liveness contract and avoid destructor
  termination.
- control-source admission microbench snapshot (`release-clang-libcxx`,
  `conflux_work_benchmarks --filter control_admission --iterations 500000`)
  currently reports:
  - `root/task_control_admission_enabled`: ~59.5 ns/iter
  - `root/task_control_admission_disabled`: ~52.2 ns/iter
  - `root/posted_control_admission_enabled`: ~58.0 ns/iter
  - `root/posted_control_admission_disabled`: ~52.0 ns/iter
  - `root/operation_control_admission_enabled`: ~57.7 ns/iter
  - `root/operation_control_admission_disabled`: ~52.2 ns/iter
- source/result factory construction now uses explicit `from_state(...)`
  static constructors (instead of friend-only private constructor access) so
  the root module compiles cleanly on both clang/libc++ and gcc/libstdc++.
- progress-capability identity now uses the root-layer `capability_id` CPO
  (`tag_invoke` customization) plus `capability_id_from_address<Derived>`
  helper inheritance in tests/benchmarks, replacing the older ad-hoc
  `.capability_id()` method convention.
- `Outcome<T>` copy assignment now uses staged copy-then-move replacement
  instead of direct variant copy assignment so throwing payload-copy paths keep
  the destination unchanged per the V10 strong-guarantee contract.
- next remaining implementation phase in this repository is complete for the
  currently available toolchains.
- downstream migration audit status:
  local scans of `/home/claudiu/conflux_dev/httpclient` and
  `/home/claudiu/conflux_dev/initial` show no current `from_root_*` /
  `to_root_*` bridge API usage, so there is no direct adapter-removal follow-up
  pending there at this time.
