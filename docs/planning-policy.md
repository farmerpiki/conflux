# Planning Policy

`todo/proposal_state.md` is the source of truth for branch selection. Use
`todo/parallel_priority_plan.md` only after checking proposal state.

Completed proposals and TODO files should be removed or archived instead of
retained as live planning records. If rationale still matters temporarily, the
file must be marked historical near the top, must be listed as `DONE` in
`todo/proposal_state.md` when it describes a landed branch, and first-contact
docs must not route users to it. Superseded proposal files should not remain in
`proposals/` or `todo/` unless they have an explicit historical/archive state
note.

New implementation proposals must include:

- `Status:`
- `Branch:`
- checklist-style acceptance checks
- the expected follow-up lane

First-contact docs must not route new users through TODO or proposal files.
