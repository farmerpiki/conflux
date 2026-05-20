# conflux::db Remaining Work

Status: open inventory

Completed pipeline implementation details were pruned. Runtime and benchmark
evidence belongs in release evidence manifests.

## Open

- COPY API: defer until an in-tree consumer or benchmark makes it useful.
- Single-row streaming: blocked on a framework-level multi-shot stream/channel
  primitive. DB must not invent a local stream abstraction.

## Done Reference

Pipeline wire mode is implemented. Live PostgreSQL evidence and benchmark
artifacts are release evidence, not TODO text.
