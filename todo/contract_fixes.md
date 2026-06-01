# Contract / Design-Rule Fixes

Status: open TODO

Current component and package contracts live in `docs/component-map.md`,
`docs/package-consumption.md`, and `CMakeLists.txt`.

## Open

- [x] Public API alias cleanup: removed the remaining file-sync compatibility
  spelling with no alias; exported shorthand alias scan is clean.
- [x] API naming pass: aligned the remaining file-sync exported temp-file type
  with the blocking naming model before freezing preview docs.
- Profiling-gated: ring hot/cold layout; verify with profiling before adding more padding or
  layout churn.
