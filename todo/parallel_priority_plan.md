# Parallel Implementation Priority Plan

Status: active branch checklist

Use `todo/proposal_state.md` first, then this file for parallel-safe work
selection. Completed branches are removed from this file instead of retained as
historical planning notes.

## Active Branches

| Priority | Branch | Parallel safety | Checklist |
|---|---|---|---|
| DONE | `work/composable-race` | `src/work/race.cxx`, `tests/work_race_test.cxx`, `docs/conflux-work-race-api.md`. | [x] Root-layer reason propagation. [x] Ready-callback exclusivity tests. [x] Callback lifetime/cleanup tests. [x] Progress-domain docs/tests. [x] Capability-safe extraction tests. [x] Live N-way race. |
| DONE | `simd/dispatch-independence-stage1` | `cmake/ConfluxOptions.cmake`, `cmake/ConfluxBuildChecks.cmake`, `scripts/check-simd-direct-shape.py`. | [x] Invalid-selection rejection. [x] AUTO-to-DIRECT resolution. [x] Direct object-shape checks. [x] Runtime probe behavior. [x] Scalar build configuration. |
| DONE | `uring/iopoll-static-evidence` | `src/file_io/iopoll.cxx`, `tests/file_io_test.cxx`, `benchmarks/storage_read_bench.cxx`. | [x] Storage-read benchmark gate exists. [x] IOPOLL remains storage-only. [x] HTTP/static adoption remains blocked until evidence exists. |
| DONE | `docs/client-streaming-polish` | Docs/client API plus one HTTP e2e test; no perf/build evidence work. | [x] Handler placement docs. [x] First-contact typed-helper guidance. [x] Timeout classification. [x] Work/UI guidance. [x] JSON literal design. [x] Blocking client response streaming. |

## Component Cleanup

- [x] Public API alias cleanup: removed remaining preview compatibility spelling
  with no alias.
- [x] Remaining `blocking_*` / `sync_*` / `async_*` renames: completed the
  remaining component-local file-sync type rename without a compatibility alias.
- [x] Router dense route-table HEAD benchmark coverage before more dispatch
  changes.
- Deferred release gate: full compiler/test/example/package matrix before tagging.

## Explicit Non-Branches

- Performance proof repository and final public proof capture are out of scope.
- Future/deferred items are out of scope for this pass.
- Completed implementation proposals should stay deleted.
- Broad source splits are not work items unless a live component TODO names the
  boundary and acceptance checks.
