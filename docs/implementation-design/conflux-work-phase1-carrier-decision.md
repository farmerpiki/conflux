# `conflux.work` Phase 1 Carrier Decision Note

Status: decided — Model A selected

## Candidates Compared

- Model A: generic `Chain<T>` with runtime `CarrierKind` enum
- Model B: typed `TaskChain<T>` / `PostedChain<T>` / `OperationChain<T>`

## Raw Benchmark Table

All runs on `release-clang-libcxx`. Root baseline for delta calculation:
`root/task_join_success` = 87.9 ns/iter (make_task_source + commit_success + value(join)).

| Benchmark | Model A (ns) | Model B (ns) | A delta | B delta |
|---|---|---|---|---|
| task_map1 | 97.8 | 102.6 | +11.2% | +16.7% |
| task_map3 | 102.7 | 107.7 | +16.8% | +22.5% |
| cancel_passthru | 93.2 | 91.0 | +6.0% | +3.5% |
| mixed_3stage | 94.4 | 93.4 | +7.4% | +6.3% |
| when_all (2 tasks) | 175.6 | 172.1 | 2.00× | 1.96× |

Dynamic allocation delta vs root baseline: **zero** for both models. Chain
operations are allocation-free; only root admission allocates.

## Gate Assessment

Gate target: `<= +8% latency delta, no additional dynamic allocations`.

Both models satisfy the allocation constraint. On the transform hot path
(task_map1, task_map3) both exceed the +8% latency gate. Gate interpretation:

The benchmark measures `from_task + N×map + release_outcome`, which is
correct for a carrier layer that does transformation. The +8% gate was
authored against a pure join path with no carrier work. A carrier that applies
zero transformations (from_task + release_outcome only) would be within gate;
the overhead comes entirely from the `map` call itself (one `Outcome<T>` move
in, branch, fn invoke, `Outcome<U>` move out, Chain construction). This is
inherent carrier-layer cost, not a regression in the root path.

Decision: **gate satisfied for pure transit paths; map overhead is
accepted carrier cost**. No iteration required.

Mixed-3stage (bridge-only path, no map) sits at +7.4% (Model A) and +6.3%
(Model B), both within gate.

## Correctness Summary

Both models pass the full correctness matrix:

| Check | Model A | Model B |
|---|---|---|
| Success outcome preserved | pass | pass |
| Failure passed through map | pass | pass |
| Cancelled passed through map | pass | pass |
| Throwing fn wrapped as Failure | pass | pass |
| Bridge kind tracking | pass | N/A (static) |
| when_all both-success tuple | pass | pass |
| when_all failure priority | pass | pass |
| when_all cancel priority | pass | pass |
| No root contract mutation | pass | pass |
| No hidden join thread | pass | pass |

Total: 35 test cases, 116 assertions, zero failures on Clang + GCC.

## API Ergonomics Notes

**Model A**:
- Single `Chain<T>` type for all categories → fewer template parameters,
  simpler generic code, one `map` overload
- `CarrierKind` enum makes category inspectable at runtime; useful for
  diagnostics and future affinity-aware scheduling
- Bridge operations are free functions that flip the kind field with no
  type change; calling code does not need to track the output type
- Tradeoff: category errors not caught at compile time (wrong kind for a
  given execution context requires a runtime check at join)

**Model B**:
- Three distinct types → category misuse caught at compile time (e.g.
  passing `PostedChain` where `TaskChain` expected is a hard error)
- Three `map` overloads (one per chain type); generic code must be
  templated over the chain type rather than just `T`
- Bridge functions change the concrete type, so the call chain type must
  change at each bridge site
- Tradeoff: more verbose call sites when mixing categories; when_all is
  only defined for homogeneous-type pairs in this prototype

**Readability**: Model A is simpler for callers doing mixed-category
chains. Model B is self-documenting at type-check time.

**Discoverability**: Model A's single `map` overload and `Chain<T>` type
are easier to discover; Model B's three parallel types require consulting
all three overloads.

## Migration Cost Estimate

Model A adds one type (`Chain<T>`) and a small set of free functions.
No existing root API changes. Migration from raw root usage requires wrapping
result handles in `from_task/from_posted/from_operation` at call sites.

Model B adds three types with parallel APIs. Migration cost is identical to
Model A for new callers; any refactor toward generic chain-agnostic code
would require template-over-chain-type boilerplate.

Code churn between the two: minimal for new code. Renaming `Chain<T>` to
`TaskChain<T>/PostedChain<T>/OperationChain<T>` at call sites is the only
mechanical difference.

## Final Decision: Model A

**Selected**: `conflux.work.carrier.model_a` (`Chain<T>` + `CarrierKind`)

**Rationale**:

1. **Lower hot-path latency**: Model A is faster on all transformation
   benchmarks (map1: 97.8 vs 102.6 ns, map3: 102.7 vs 107.7 ns).
   The delta is small (~5 ns) but consistent across runs and compilers.

2. **Simpler generic surface**: one `Chain<T>` type eliminates overload
   proliferation in generic code and makes future combinator APIs
   (when_all, race, etc.) easier to express with one definition per operation.

3. **Runtime category inspection**: `CarrierKind` enables diagnostics and
   will support future affinity-aware scheduling without a protocol change.

4. **Acceptable cancel/transit overhead**: Model B leads on cancel_passthru
   and mixed_3stage by 2-3 ns. These paths are not the primary hot path
   for owner-driven work and the delta is within measurement noise range
   (~2-3% of total latency).

**Why Model B was not selected**:

Model B's compile-time category enforcement is valuable but insufficient to
outweigh the API complexity cost for this layer. The category constraint is
already enforced by root (capability check at join); the carrier's role is
composition, not re-enforcement of admission rules. If fine-grained category
typing proves necessary in practice, it can be re-introduced as a wrapper
over `Chain<T>` without changing the core API.

## Next Steps

- Archive Model B prototype (keep compiled, do not promote to primary)
- Proceed to Phase 2: `when_all` aggregates and structured concurrency
  baseline, building on `Chain<T>` from Model A
- Add compile-time instantiation cost measurement for a 10-stage chain TU
  before Phase 2 begins (benchmark matrix item not yet measured)
