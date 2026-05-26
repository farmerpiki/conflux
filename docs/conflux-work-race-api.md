# Composable Work Race API

`conflux.work.race` provides same-type races over root `Task<T>` and
`TaskJoinHandle<T>` values, plus ready `carrier::Chain<T>` outcomes. It is a
coordination layer over existing task controls; it does not own thread or ring
execution.

## Participants

Use `race::candidate(label, task)` for value-producing work and
`race::trigger(label, task, reason)` for non-value work that should win by
committing a cancellation reason, such as a deadline task.

Labels are borrowed by default. Use `race_owned_labels()` when labels are built
dynamically or do not outlive the race result.

## Winner Policy

`winner_policy::first_completion` selects the first participant that reaches a
terminal outcome.

`winner_policy::first_success` ignores failed or cancelled value candidates
until a success wins. If all value candidates fail, the result is a failure. If
more than one value candidate failed, the failure contains `race_aggregate_error`.

Triggers are immediate winners when they complete successfully. Their result is
reported as `Cancelled{trigger.reason}`.

## Loser Policy

`loser_policy::leave_running` leaves unfinished participants alone.

`loser_policy::request_cancel` requests cancellation on unfinished losers and
returns as soon as the winner is selected.

`loser_policy::request_cancel_and_wait` requests cancellation and returns only
after all live participants have reached terminal completion. This is the
default because it preserves ownership and callback lifetime without detaching
unfinished loser work.

`loser_cleanup_policy` and `loser_cleanup_budget` are part of the public shape
reserved for owner-bound timer integration. The current bare race primitive does
not enforce cleanup deadlines by itself because it intentionally does not own a
progress domain or timer backend.

Cancelling the returned race task forwards the cancellation reason to every live
participant and completes the race task as cancelled with the same reason.

## Ownership

The base API is allocation-light and borrows labels. `race_owned_labels()` copies
only labels, then rebinds winner and aggregate-diagnostic label views to owned
storage in the returned result.
