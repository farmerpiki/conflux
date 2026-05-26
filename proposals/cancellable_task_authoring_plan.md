# Cancellable Task Authoring Plan

## Objective

Make cancellation first-class for user-authored work tasks while keeping
`race::race(...)` as the low-level allocation-light primitive.

## Scope

In scope:

- `Task` coroutine cancellation should commit `Outcome::cancelled`, not failure.
- Add root task-authoring cancellation context.
- Add `make_cancellable_task` for cancellable task bodies.
- Add explicit cancellable work-pool offload.
- Update the race first-responder example to use task-native cancellation.
- Add race sugar only after root cancellation semantics are proven.

Out of scope:

- Replacing `race::race(...)`.
- Adding `race_tasks_to_completion`.
- Making cancellation preemptive or thread-killing.

## Implementation Order

1. Special-case `CancelledError` in `Task::promise_type::unhandled_exception()`.
   A thrown `CancelledError` inside `Task<T>` must commit `Cancelled(reason)`.
2. Add `root::Cancellation` as a cheap task-body-scoped view.
   It should expose `requested()`, `reason()`, `stop_token()`, and
   `throw_if_requested()`.
3. Add `root::make_cancellable_task` for sync bodies:
   `Fn(Cancellation) -> T`.
4. Add flattened async-body support:
   `Fn(Cancellation) -> Task<T>`.
5. Implement structured child binding for `Cancellation::await(child)`.
   Internally treat this as core cancellation-forwarding infrastructure:
   `bind_child_for_cancellation(parent_state, child_control)` and
   `clear_child_for_cancellation(parent_state, token)`.
6. Add `async_run_cancellable_on(Target&, Fn)` for sync offload bodies.
7. Update `examples/advanced/work_race_first_responder.cxx` to use
   `async_run_cancellable_on` and ordinary `race::candidate(...)`.
8. Add `race::task(...)` sugar after root cancellation tests are stable.
   `race::task_on(...)` needs a module-layering decision because
   `async_run_cancellable_on` currently lives in the `conflux.work:api`
   partition rather than `conflux.work.race`.

## Structured Child Binding Contract

`Cancellation::await(child)` is not convenience sugar. It binds parent task
cancellation to the currently awaited child:

- parent cancellation stores the reason in parent control;
- if a child is bound, the same reason is requested on the child control;
- child owner performs real cleanup;
- parent resumes only after child terminal completion.

Required races:

- parent cancelled before child await starts;
- parent cancelled while child is awaited;
- child completes while parent cancellation forwards;
- parent exits after child terminal completion.

The cancel hook must not capture stack awaiter state. Active child state should
live in parent task state, use a generation token for clear, and hold a child
control value whose lifetime is independent of the awaiter stack.

## Test Checklist

- Throwing `CancelledError{deadline}` inside `Task<T>` commits cancelled.
- Throwing `CancelledError{shutdown}` inside `Task<void>` commits cancelled.
- `make_cancellable_task` sync body returns success.
- `make_cancellable_task` sync body throws ordinary exception as failure.
- `make_cancellable_task` sync body throws `CancelledError` as cancelled.
- Cancelling before body checks cancellation exposes requested reason.
- Async-body `make_cancellable_task` flattens child success.
- Child failure propagates as failure.
- Child cancellation propagates as cancellation.
- Parent cancelled before `cancel.await(child)` forwards to child.
- Parent cancelled while awaiting child forwards to child.
- Child completion racing parent cancellation has one terminal outcome and no UAF.
- Active child binding is cleared after child terminal completion.
- Race first-responder example cancels losers without raw `TaskSource`.

## Progress

- [x] Step 1: `CancelledError` commits cancelled in `Task` coroutine promises.
- [x] Step 2: `root::Cancellation` view.
- [x] Step 3: sync `make_cancellable_task`.
- [x] Step 4: async-body flattening.
- [x] Step 5: structured child binding.
- [x] Step 6: `async_run_cancellable_on`.
- [x] Step 7: first-responder example update.
- [x] Step 8: `race::task` sugar.
- [ ] Step 8b: decide layering for `race::task_on`.
