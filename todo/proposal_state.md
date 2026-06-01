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
| DONE | `work/composable-race` | `proposals/conflux_composable_race_full_proposal.md`, `src/work/race.cxx`, `tests/work_race_test.cxx`, `docs/conflux-work-race-api.md` | [x] Implemented: reason propagation, exclusive ready-callback registration, callback lifetime/cleanup, capability-safe extraction, and live N-way race are covered by the race implementation and tests. |
| DONE | `simd/dispatch-independence-stage1` | `proposals/simd_dispatch_independence_stage1_proposal.md`, `cmake/ConfluxOptions.cmake`, `cmake/ConfluxBuildChecks.cmake`, `scripts/check-simd-direct-shape.py` | [x] Validated invalid-selection rejection, AUTO/DIRECT resolution, direct object-shape checks, runtime probe configuration, and scalar configuration. |
| DONE | `uring/iopoll-static-evidence` | `proposals/t2_c_iopoll_ring_proposal.md`, `src/file_io/iopoll.cxx`, `tests/file_io_test.cxx`, `benchmarks/storage_read_bench.cxx` | [x] Storage-only primitive and storage-read benchmark gate exist; HTTP/static adoption remains blocked until storage-read bottleneck evidence exists. |
| DONE | `docs/client-streaming-polish` | `docs/conflux-http-client-api.md`, `docs/conflux-work-root-api.md`, `docs/json-api.md`, `src/net/client.cxx`, `tests/http_e2e.cxx`, `todo/root_doc_claims_to_triage.md` | [x] Implemented the non-perf TODO priority pass: HTTP placement docs, first-contact typed-helper guidance, timeout classification, work-runtime/UI guidance, JSON literal design, and blocking client response streaming. |

## Active Cleanup TODOs

| Area | TODO state |
|---|---|
| Public API aliases | [x] Removed remaining public preview compatibility spelling `TemporaryFileSync` with no alias; shorthand alias scan shows no exported `S`/`SV`/`SP`/`Opt`/`Vec`/`Map` public aliases. |
| API naming | [x] Renamed the remaining exported file-sync suffix type to `BlockingTemporaryFile`; focused build and compile-fail guard pass. |
| Release verification | Deferred release gate: run the configured compiler/test/example/package lanes before tagging, outside this implementation pass. |
| Router coverage | [x] Added `micro/router_dispatch_dense_head_exact` in `benchmarks/router_bench.cxx`; verified with `conflux_benchmarks --list | rg dense_head` and a one-iteration smoke run. |

## Selection Rules

- Pick the first P1 item unless already working in another component.
- Future/deferred items are intentionally out of scope for this implementation pass.
- Treat source docs above as TODO lists; mark checklist entries done as they land.
- Do not recreate completed proposal files as rationale-only documents.
- Do not start broad source splits or API alias removal outside the final release
  cleanup lane.
