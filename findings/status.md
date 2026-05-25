# Findings status

Current review set: `findings/1.md` through `findings/7.md` after
`71aabad remove findings`.

## Complete

- `findings/4.md` P0 structured-log checks: complete. Raw structured-log tests,
  the observability golden e2e, and HTTP facade observability tests now parse log
  lines as JSON, assert exact fields/types, assert redacted header fields by JSON
  pointer, and recursively check that secret values are absent. The quote
  escaping case now sends an actual quote-bearing request target over a raw TCP
  request instead of testing `/q`.

## Verification

- `cmake --build --preset release-clang-libcxx --target conflux_tests
  conflux_observability_golden_e2e conflux_http_facade_tests` completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "structured_log:|observability golden e2e|http facade: observability"`
  completed: 10/10 passed.

## Accepted / In Scope

- Non-performance correctness, lifetime-safety, copy-avoidance, and ergonomics
  claims in the current finding files remain in scope when they are not marked
  defer/do-not-touch/not-yet and do not require a massive rewrite.
- Performance work is now in scope only after those items are cleared or
  explicitly deferred, and only with representative benchmarks using the
  documented compare-bins/perf methodology.

## Deferred / Perf-Gated

- Hot-path performance proposals from `findings/3.md` and performance-sensitive
  lazy/ranges changes from `findings/1.md` require representative benchmark
  coverage first. If no benchmark exists for a path, add one before evaluating a
  performance implementation.
