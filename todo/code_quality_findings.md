# Code Quality Findings TODO

Status: open TODO

Use `todo/proposal_state.md` for branch selection. This file keeps only active
quality work.

## Open

- [x] Public API alias cleanup: removed the remaining `TemporaryFileSync` preview
  compatibility spelling with no alias and added a compile-fail guard.
- [x] API naming pass: renamed the remaining file-sync exported suffix type to
  `BlockingTemporaryFile`.
- Deferred release gate: run the configured compiler/test/example/package matrix
  before tagging.
- [x] Router benchmark coverage: added `micro/router_dispatch_dense_head_exact` and verified it builds, lists, and smoke-runs.
  more dispatch changes.

## Deferred

- [ ] Split `src/work/root.cxx` only after allocation/control-block profiling.
- [ ] Further source splits must be branch-local and justified by a real boundary,
  not line count.
