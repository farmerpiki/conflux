# conflux::db Remaining Work

Status: open TODO

This file tracks only DB work that remains useful for future implementation.

## Open

- Deferred: COPY API waits until an in-tree consumer or non-perf product need makes it useful.
- Blocked: single-row streaming waits on a framework-level multi-shot stream/channel
  primitive. DB must not invent a local stream abstraction.
