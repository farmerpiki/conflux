# Migration Guide — examples/

No legacy `conflux.work` API usage found in `examples/` during the Phase 0
inventory sweep. No migration needed for this group.

## Build coverage

Example targets are built by default when their feature dependencies are enabled.
Disable them explicitly with `-DCONFLUX_BUILD_EXAMPLES=OFF` when a configure is
only meant to build libraries/tests/benchmarks.

The aggregate build target is:

```bash
cmake --build /tmp/conflux/<preset> --target conflux_examples
```

When `CONFLUX_BUILD_TESTS=ON`, CTest also exposes a build-only gate:

```bash
ctest --test-dir /tmp/conflux/<preset> -R '^examples/compile$'
```

This gate compiles all example executables available in the active feature set;
it does not run server examples that would block waiting for connections.

## Before / After pairs

_None — this section is populated as E1.2 PRs land._
