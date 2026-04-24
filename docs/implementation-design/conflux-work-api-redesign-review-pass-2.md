# `conflux.work` API Redesign Review Pass 2

This file captures the findings and summary from the second review pass of
`conflux-work-api-redesign.md`, combining:

- one pure API critique
- one codebase-aware critique

## Findings

1. `Operation<T>` is still too broad.

The replacement draft improves on the first one, but it still compresses
materially different owner-driven models into one category.

- `RingLane` work is queue-node work.
- `file_io` is CQE-slot / stale-generation / multishot driven.
- `db` is an owner-thread libpq state machine with poll-arm phases.

Those are not the same shape. A single `Operation<T>` category is still hiding
real differences in:

- snapshot semantics
- cancellation behavior
- driving model
- completion affinity

Relevant code:

- [work.cxx#L843](/home/claudiu/conflux_dev/api-fixes/src/work.cxx#L843)
- [file_io.cxx#L568](/home/claudiu/conflux_dev/api-fixes/src/file_io/file_io.cxx#L568)
- [db.cxx#L928](/home/claudiu/conflux_dev/api-fixes/src/db/db.cxx#L928)

2. Cross-category coroutine and combinator behavior is still not coherent.

The pure API review found that `and_then` is internally contradictory:

- the signature comment says it may return `Task<U>` or `Operation<U>`
- the rules say category is preserved
- the bridge section says category changes must be explicit

As written, a pipeline like `Task<T> | and_then(fn returning Operation<U>)` has
no defined meaning.

Relevant doc sections:

- [conflux-work-api-redesign.md#L410](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L410)
- [conflux-work-api-redesign.md#L417](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L417)
- [conflux-work-api-redesign.md#L419](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L419)
- [conflux-work-api-redesign.md#L433](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L433)

The codebase-aware review found a deeper problem: current coroutine usage often
assumes one coroutine can await any `Flow`-backed source. Forcing `file_io` and
`db` into `Operation<T>` would push TLS and protocol code into owner-bound
coroutines transitively.

Relevant code:

- [coroutines.cxx#L21](/home/claudiu/conflux_dev/api-fixes/examples/coroutines.cxx#L21)
- [db_coro_bench.cxx#L56](/home/claudiu/conflux_dev/api-fixes/benchmarks/db_coro_bench.cxx#L56)
- [tls.cxx#L332](/home/claudiu/conflux_dev/api-fixes/src/net/tls.cxx#L332)

3. The ownership model still makes `detach()` feel like the normal path.

The pure API review called out that the recommended pattern:

1. create result
2. extract handle
3. store handle
4. `detach(...)`

effectively makes detached work the common case for long-lived server flows.

That means:

- typed outcomes are discarded
- failure reporting moves to a detached sink
- the linear result object becomes mostly a ceremony layer

Relevant doc sections:

- [conflux-work-api-redesign.md#L195](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L195)
- [conflux-work-api-redesign.md#L257](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L257)
- [conflux-work-api-redesign.md#L717](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L717)

The codebase-aware review found this is not theoretical: router deferred
response paths already build terminal async chains and drop the result object
immediately.

Relevant code:

- [router.cxx#L2778](/home/claudiu/conflux_dev/api-fixes/src/net/router.cxx#L2778)
- [router.cxx#L2927](/home/claudiu/conflux_dev/api-fixes/src/net/router.cxx#L2927)
- [router.cxx#L3040](/home/claudiu/conflux_dev/api-fixes/src/net/router.cxx#L3040)
- [http_server.cxx#L3149](/home/claudiu/conflux_dev/api-fixes/src/net/http_server.cxx#L3149)

4. Cancellation is better than in the previous draft, but still incomplete.

The pure API review found a missing liveness contract:

- `request_cancel()` is only a request
- producer hooks are optional
- cooperative cancellation can be ignored indefinitely
- aggregates like `race()` / `when_all()` are written as if sibling
  cancellation is enough to finish them

The document still needs to say whether cancellation guarantees eventual
terminal completion, or whether cancellation can remain in-flight forever.

Relevant doc sections:

- [conflux-work-api-redesign.md#L487](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L487)
- [conflux-work-api-redesign.md#L530](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L530)
- [conflux-work-api-redesign.md#L557](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L557)

The codebase-aware review found a concrete mismatch: `db::cancel_inflight`
currently completes from a `WorkPool` worker even though the connection itself
is owner-bound. That breaks the hard `Operation<T>` owner-affinity story unless
`db` has an explicit exception or marshal-back rule.

Relevant code:

- [db.cxx#L1044](/home/claudiu/conflux_dev/api-fixes/src/db/db.cxx#L1044)

5. Affinity is still not fully specified outside raw `co_await`.

The replacement draft makes `co_await` resume rules much clearer, but it still
does not state where combinator callbacks run:

- `then`
- `and_then`
- `on_failure`
- `on_cancel`

For this design, execution context is a core semantic, not an implementation
detail. If combinator affinity is not explicit, `Operation<T>` still has room
for off-owner bugs.

Relevant doc sections:

- [conflux-work-api-redesign.md#L409](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L409)
- [conflux-work-api-redesign.md#L456](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L456)
- [conflux-work-api-redesign.md#L473](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L473)

The codebase-aware review also found that initial coroutine start semantics are
still unspecified, and that matters for owner-driven work because eager vs lazy
start changes what `block_on(owner, op)` actually has to drive.

Relevant current behavior:

- [work.cxx#L1467](/home/claudiu/conflux_dev/api-fixes/src/work.cxx#L1467)
- [file_io.cxx#L2806](/home/claudiu/conflux_dev/api-fixes/src/file_io/file_io.cxx#L2806)

6. The rejection/failure boundary is cleaner than before, but still leaks
subsystem-specific knowledge.

The design says:

- rejection means admission failed and no async value exists
- accepted work only ends in success, failure, or cancellation

That is a good simplification, but it is weakened by subsystem carve-outs like
file-I/O submission/resource failures being treated as `Failure` instead of
`Reject`.

That means users still need subsystem-specific knowledge to know whether
\"could not even start\" is synchronous or asynchronous.

Relevant doc sections:

- [conflux-work-api-redesign.md#L162](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L162)
- [conflux-work-api-redesign.md#L371](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L371)
- [conflux-work-api-redesign.md#L685](/home/claudiu/conflux_dev/api-fixes/docs/implementation-design/conflux-work-api-redesign.md#L685)

The codebase-aware review also found this is awkward for `process`, where the
useful domain shape is still probably `Task<expected<...>>` and admission
rejection just moves one failure branch to the front.

Relevant code:

- [process.cxx#L518](/home/claudiu/conflux_dev/api-fixes/src/process.cxx#L518)
- [process.cxx#L624](/home/claudiu/conflux_dev/api-fixes/src/process.cxx#L624)
- [process_test.cxx#L217](/home/claudiu/conflux_dev/api-fixes/tests/process_test.cxx#L217)

7. The one-control-block-allocation target is still asserted, not explained.

The codebase-aware review called this out directly: the current combinator
implementation allocates stage state aggressively, and the draft does not yet
show how the new model avoids similar per-stage state in real router call
patterns.

Relevant current code:

- [work.cxx#L1077](/home/claudiu/conflux_dev/api-fixes/src/work.cxx#L1077)
- [work.cxx#L1116](/home/claudiu/conflux_dev/api-fixes/src/work.cxx#L1116)
- [work.cxx#L1144](/home/claudiu/conflux_dev/api-fixes/src/work.cxx#L1144)

## Summary

The replacement draft is better than the first redesign draft. The critiques
did not reject the move toward more explicit models. They rejected where the
new split still does not match the real shapes in conflux.

The main unresolved problems are:

1. `Operation<T>` is still too broad.
2. Cross-category coroutine and combinator behavior is not coherent yet.
3. The ownership model still makes `detach()` feel like the normal path.
4. Cancellation and affinity are still not fully specified for aggregates,
   continuations, and mixed completion paths.

The main takeaway from review pass 2 is:

The redesign is improving, but the boundary should probably not be just
`Task<T>` vs `Operation<T>`. Conflux likely needs a more precise split across:

- autonomous scheduled work
- explicitly posted owner work
- driver/CQE/state-machine driven operations

`process` looks comparatively low-risk.

The high-risk areas are:

- `file_io`
- `db`
- TLS / protocol coroutines
- router / `http_server` deferred-response ownership
