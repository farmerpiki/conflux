# Contract / Design-Rule Fixes

Status: open inventory

Completed component/package split chains were pruned. Current component and
package contracts live in `docs/component-map.md`, `docs/package-consumption.md`,
and `CMakeLists.txt`.

## Open

- Public API alias cleanup: remove exported shorthand aliases from public
  signatures and first-contact docs in the final release cleanup lane.
- API naming pass: align remaining public APIs with `blocking_*`, `sync_*`, and
  `async_*` after the public surface is otherwise settled.
- Ring hot/cold layout: verify with profiling before adding more padding or
  layout churn.

## Done Reference

Completed: modular target split, package export placement, file sync/map
independence, JSON file convenience, HTTP component split, dependency-edge
cleanup, stream-vocabulary cleanup, and package config dependency restoration.
