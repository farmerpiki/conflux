# Parallel Implementation Priority Plan

Status: active branch checklist

Use `todo/proposal_state.md` first, then this file for parallel-safe work
selection. Completed branches are removed from this file instead of retained as
historical planning notes.

## Active Branches

| Priority | Branch | Parallel safety | Checklist |
|---|---|---|---|
| P1 | `work/composable-race` | Touches root work/cancellation internals; keep one owner. | [ ] Land root-layer reason propagation. [ ] Add ready-callback exclusivity tests. [ ] Add callback lifetime/quiescence tests. [ ] Add progress-domain tests. [ ] Add capability-safe extraction tests. [ ] Add live N-way race only after those contracts are green. |
| P2 | `simd/dispatch-independence-stage1` | Build/CMake plus named SIMD call sites; keep crypto ISA policy separate. | [ ] Validate invalid-selection rejection. [ ] Validate AUTO-to-DIRECT resolution. [ ] Validate direct object-shape checks. [ ] Validate runtime probe behavior. [ ] Validate scalar build. |
| P2 | `uring/iopoll-static-evidence` | Storage primitive exists; HTTP/static adoption must stay isolated. | [ ] Prove HTTP static serving is storage-read-bound before any adoption. [ ] Keep IOPOLL storage-only. [ ] Preserve owner-ring continuation affinity if a consumer is added. |
| DEFERRED | `http/streaming-upload-api` | Future server API; do not start during prerelease API cleanup. | [ ] Design bounded-memory request body streaming. [ ] Design multipart streaming. [ ] Design spill-to-file and backpressure semantics. |

## Component Cleanup

- [ ] Public API alias cleanup: final release cleanup only.
- [ ] Remaining `blocking_*` / `sync_*` / `async_*` renames: component-local
  patches only, with compatibility aliases kept until final cleanup.
- [ ] Router dense route-table HEAD benchmark coverage before more dispatch
  changes.
- [ ] Full compiler/test/example/package matrix before tagging.

## Explicit Non-Branches

- Performance proof repository and final public proof capture are out of scope.
- Completed implementation proposals should stay deleted.
- Broad source splits are not work items unless a live component TODO names the
  boundary and acceptance checks.
