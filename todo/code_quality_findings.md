# Code Quality Findings TODO

Status: open TODO

Use `todo/proposal_state.md` for branch selection. This file keeps only active
quality work.

## Open

- Deferred release gate: run the configured compiler/test/example/package matrix
  before tagging.

## Deferred

- Profiling-gated: split `src/work/root.cxx` only after allocation/control-block profiling.
- Deferred source hygiene: further source splits must be branch-local and justified by a real boundary,
  not line count.
