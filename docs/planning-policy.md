# Planning Policy

`todo/proposal_state.md` is the source of truth for branch selection. Use
`todo/parallel_priority_plan.md` only after checking proposal state.

Completed proposals and TODO files should be removed instead of retained as
rationale-only planning records. If rationale still matters, move it into the
current design/API document that owns the behavior. Superseded proposal files
should not remain in `proposals/` or `todo/`.

New implementation proposals must include:

- `Status:`
- `Branch:`
- checklist-style acceptance checks
- the expected follow-up lane

First-contact docs must not route new users through TODO or proposal files.
