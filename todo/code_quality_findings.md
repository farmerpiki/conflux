# Code Quality Findings TODO

Status: open TODO

Use `todo/proposal_state.md` for branch selection. This file keeps only active
quality work.

## Open

- [ ] Public API alias cleanup: remove exported shorthand/global compatibility names
  from the public preview surface after component boundaries settle.
- [ ] API naming pass: finish the `blocking_*` / `sync_*` / `async_*` cleanup after
  alias candidates stop moving.
- [ ] Full matrix verification: run the configured compiler/test/example/package
  lanes before tagging.
- [ ] Router benchmark coverage: add dense route-table HEAD cases before accepting
  more dispatch changes.

## Deferred

- [ ] Split `src/work/root.cxx` only after allocation/control-block profiling.
- [ ] Further source splits must be branch-local and justified by a real boundary,
  not line count.
