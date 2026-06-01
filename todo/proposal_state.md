# TODO / Proposal State Index

Status: active TODO index

Purpose: keep branch selection and TODO cleanup tied to live work only. Use this
file with `todo/parallel_priority_plan.md`; completed proposals and TODO files
are removed instead of kept as historical planning records.

Performance proof and external proof-repository work are out of scope for this
planning index. Benchmark evidence can still gate a risky implementation, but
there is no release-proof branch here.

## Active Implementation TODOs

| Priority | Branch | Source doc | TODO state |
|---|---|---|---|
| P1 | `work/composable-race` | `proposals/conflux_composable_race_full_proposal.md` | [ ] Finalize staged implementation after root cancellation contracts and owner-local integration rules stay green under full tests. |
| P2 | `simd/dispatch-independence-stage1` | `proposals/simd_dispatch_independence_stage1_proposal.md` | [ ] Validate direct/runtime SIMD selection semantics and object-shape checks. |
| P2 | `uring/iopoll-static-evidence` | `proposals/t2_c_iopoll_ring_proposal.md`, `todo/io_uring_remaining.md` | [ ] Keep HTTP/static adoption blocked until storage-read bottleneck evidence exists. |
| DEFERRED | `http/streaming-upload-api` | `todo/server_gaps.md`, `docs/http-server-api.md` | [ ] Add a bounded-memory request body and multipart upload streaming API with backpressure and optional spill-to-file after prerelease API/docs settle. |

## Active Cleanup TODOs

| Area | TODO state |
|---|---|
| Public API aliases | [ ] Remove exported shorthand/global compatibility names from the public preview surface after component boundaries settle. |
| API naming | [ ] Finish the `blocking_*` / `sync_*` / `async_*` cleanup after alias candidates stop moving. |
| Release verification | [ ] Run the configured compiler/test/example/package lanes before tagging. |
| Router coverage | [ ] Add dense route-table HEAD benchmark coverage before accepting more dispatch changes. |

## Selection Rules

- Pick the first P1 item unless already working in another component.
- Treat source docs above as TODO lists; mark checklist entries done as they land.
- Do not recreate completed proposal files as rationale-only documents.
- Do not start broad source splits or API alias removal outside the final release
  cleanup lane.
