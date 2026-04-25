# `conflux.work` Phase 1 Execution: Carrier Decision And MVP

Status: active execution plan

Source alignment:

- `conflux-work-api-redesign-proposal-v10.md`
- `conflux-work-api-redesign-deferred-plan.md`

## Objective

Select and ship one carrier/composition model above `conflux.work.root` that:

- supports mixed async chains without hidden helper runtime
- preserves owner hot-path constraints
- keeps root-layer contracts unchanged

## Candidate Models

## Model A: Generic Carrier Above Root Categories

Shape:

- one composition type abstracts `Task/Posted/Operation`
- bridges into root values only at admission and terminal boundaries

Hypothesis:

- best API uniformity, potentially higher indirection cost

## Model B: Root-Coroutine Types + Explicit Bridges

Shape:

- category-specific coroutine carriers
- explicit bridge APIs between carriers/categories

Hypothesis:

- lower overhead and clearer affinity semantics, potentially more verbose API

## Guardrails (Non-Negotiable)

- no change to root liveness/linear-destruction contract
- no implicit context hop
- no hidden join-helper thread
- capability mismatch must remain explicit failure path
- cross-category bridges are ownership/affinity transfer only: they must not
  add hidden progress paths or extra root control blocks

## Prototype Scope

Implement minimal but equivalent prototype slices for A and B:

- value transform chain (`map` / `then` equivalent)
- error/cancel transform path
- cross-category bridge (`Task -> Posted`, `Posted -> Operation`)
- one aggregate (`when_all` wait-all only)

`when_all` in this phase must preserve the V10 locked default semantic:

- wait-all behavior only
- no implicit sibling fail-fast cancellation

## Benchmark Matrix

All runs on `release-clang-libcxx` and `debug-clang-libcxx`:

- owner-hot single-step chain latency
- mixed-category 3-stage chain latency
- cancellation request overhead in active chain
- allocation count per admitted chain
- binary/code size delta of carrier implementation
- compile-time instantiation/build cost for a representative mixed 10-stage
  chain benchmark TU

## Correctness Matrix

- deterministic callback/continuation placement by category
- cancellation propagation remains best-effort
- no leak/live-object termination regressions
- capability checks remain enforced at blocking boundaries

## Deliverables

- prototype implementation for Model A (compile-gated)
- prototype implementation for Model B (compile-gated)
- benchmark report with raw numbers + delta analysis
- decision record selecting A or B

## Decision Gate

Select model only if all are true:

1. Meets all correctness checks
2. No root contract regression
3. Owner-hot benchmark within agreed budget (initial gate target: <= +8%
   latency delta and no additional dynamic allocations versus root baseline)
4. Mixed-chain latency and cancellation overhead are acceptable

If neither qualifies, iterate once with narrowed design adjustments, then
re-run gate.

## Immediate Work Items

1. Add compile-time feature flags for carrier experiment variants.
2. Implement Model A prototype module surface.
3. Implement Model B prototype module surface.
4. Extend benchmark binary with phase1 carrier cases.
5. Add focused tests for continuation placement and cancellation behavior.
6. Publish phase1 decision note.

## Decision Note Template (Work Item 6)

- Candidate compared: Model A vs Model B
- Raw benchmark table: owner-hot, mixed-chain, cancellation, alloc count, code
  size, compile-time cost
- Correctness summary: pass/fail matrix and notable edge cases
- API ergonomics notes: readability, discoverability, migration friction
- Migration cost estimate: code churn and compatibility impact
- Final decision and rationale: include why non-selected model was rejected
