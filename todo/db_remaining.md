# conflux::db Remaining Work

Status: open TODO

This file tracks only DB work that remains useful for future implementation.

## Open

- [ ] COPY API: defer until an in-tree consumer or benchmark makes it useful.
- [ ] Single-row streaming: blocked on a framework-level multi-shot stream/channel
  primitive. DB must not invent a local stream abstraction.
