# TODO / Proposal State Index

Status: active TODO index

Purpose: keep branch selection and TODO cleanup tied to live work only. Use this
file with `todo/parallel_priority_plan.md`; completed proposals and TODO files
are removed instead of kept as historical planning records.

Performance proof and external proof-repository work are out of scope for this
planning index. Benchmark evidence can still gate a risky implementation, but
there is no release-proof branch here.

## Active Implementation TODOs

- None. Current implementation branches are complete; remaining work is gated
  cleanup, evidence, or future-surface tracking in the component TODO files.

## Historical Proposal References

These proposal files stay mentioned for the planning guard, but they are not
active implementation TODOs:

- `proposals/conflux_composable_race_full_proposal.md`: core race primitive,
  reason propagation, callback ownership, and live N-way coverage landed.
- `proposals/simd_dispatch_independence_stage1_proposal.md`: SIMD dispatch
  independence Stage 1 implementation and validation landed.
- `proposals/t2_c_iopoll_ring_proposal.md`: storage-only IOPOLL primitive,
  tests, and storage-read benchmark gate landed; HTTP/static adoption remains
  evidence-gated in `todo/io_uring_remaining.md`.

## Active Cleanup TODOs

| Area | TODO state |
|---|---|
| Release verification | Deferred release gate: run the configured compiler/test/example/package lanes before tagging, outside this implementation pass. |

## Selection Rules

- There is no current P1 implementation branch.
- Future/deferred items are intentionally out of scope for this implementation pass.
- Treat component TODO files as the source of current remaining work.
- Do not recreate completed proposal files as rationale-only documents.
- Do not start broad source splits or API alias removal outside the final release
  cleanup lane.
