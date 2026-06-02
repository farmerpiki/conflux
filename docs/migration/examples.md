# Migration Guide - examples/

No legacy `conflux.work` API usage found in `examples/` during the Phase 0
inventory sweep. No migration needed for this group.

## Build coverage

Example targets are built by default when their feature dependencies are enabled.
Disable them explicitly with `-DCONFLUX_BUILD_EXAMPLES=OFF` when a configure is
only meant to build libraries/tests/benchmarks.

The aggregate build target is:

```bash
BUILD_DIR="$(python3 scripts/cmake-preset-build-dir.py "$PWD" <preset>)"
cmake --build "$BUILD_DIR" --target conflux_examples
```

When `CONFLUX_BUILD_TESTS=ON`, CTest also exposes a build-only gate:

```bash
ctest --test-dir "$BUILD_DIR" -R '^examples/compile$'
```

This gate compiles all example executables available in the active feature set;
it does not run server examples that would block waiting for connections.

## Removed preview spellings

Current public examples use final preview names only. Historical snippets and
old external drafts may need these replacements before being copied into current
docs or quickstarts:

| Historical spelling | Current spelling |
| --- | --- |
| historical short aliases | standard C++ names such as `std::string_view`, `std::optional`, `Callable`, `std::error_code`, `std::size_t` |
| legacy async names | `async_*` coroutine/task APIs |
| legacy direct fd helpers | `blocking_*` caller-thread helpers |
| legacy socket wait helper | `sync_wait_socket_task` |

## Before / After pairs

_None — this section is populated as E1.2 PRs land._
