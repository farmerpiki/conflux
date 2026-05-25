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
- `findings/6.md` P0 multipart/header parameter extraction: complete for the
  quoted-semicolon parsing defect. `extract_param(...)` now delegates segment
  splitting to the shared `header_params(...)` parser, preserving semicolons
  inside quoted parameter values without introducing lazy ranges or extra copies
  for ordinary token/quoted values.
- `findings/6.md` P0 client chunked decoder drift: complete. Sync and async
  HTTP clients now drive the shared incremental chunked decoder instead of the
  separate client-only parser, preserving streaming state and moving the decoded
  body only on completion.
- `findings/1.md` P0 stop-aware `HttpFields` matching: complete. Added
  `for_each_value_until(...)` and the free `for_each_header_value_until(...)`
  wrapper, and migrated HTTP/1 connection/expect/transfer-encoding predicates
  to the stop-aware callbacks.
- `findings/6.md` P1 H1/H2 request derived-field drift: complete. Added shared
  `populate_request_parts(...)` for query, form, multipart, cookie, and uploaded
  file population, and routed both HTTP/1 and HTTP/2 dispatch through it.
- `findings/6.md` P1 sync/async client redirect drift: complete. Redirect
  request construction now lives in `client_wire::follow_redirect_request(...)`
  and both sync and async clients call it.
- `findings/7.md` P1 `JsonArena::reset()` hash-index resource loss: complete.
  `JsonArena` now retains the selected hash-index memory resource and recreates
  `DocumentStorage` with it after explicit reset; the existing counting-resource
  test now exercises warm-member-index allocation after reset.

## Verification

- `cmake --build --preset release-clang-libcxx --target conflux_tests
  conflux_observability_golden_e2e conflux_http_facade_tests` completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "structured_log:|observability golden e2e|http facade: observability"`
  completed: 10/10 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_http_server_helpers_tests conflux_tests` completed after multipart
  parameter extraction cleanup.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http_server_helpers: header parameter extraction|multipart/form-data quoted
  semicolon filename"` completed: 2/2 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_net_client
  conflux_net_async_client conflux_tests` completed after adding the client
  dependency on shared HTTP parse helpers and unifying chunked response decode.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http client: chunked response without trailers|http_server_helpers: chunked
  decoder"` completed: 2/2 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_http_core
  conflux_http_server_helpers conflux_http_server_helpers_tests conflux_tests`
  completed after stop-aware `HttpFields` callback cleanup.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "HttpFields zero-allocation value callbacks|http_server_helpers: (connection
  tokens|expect and transfer-encoding)"` completed: 3/3 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_http_server
  conflux_tests conflux_h2_external` completed after H1/H2 request-parts
  population cleanup.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "multipart/form-data|query param|Cookie header|h2: (POST body is echoed|GET
  with path param echoes name|GET /ping returns 200)"` completed: 22/22 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_net_client
  conflux_net_async_client conflux_tests` completed after sync/async redirect
  construction cleanup.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http client: follow_redirects|http client async: async_send follows relative
  redirects"` completed: 4/4 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_json_tests`
  completed after preserving the `JsonArena` hash-index resource across reset.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "JsonArena hash index allocations use the injected resource"` completed: 1/1
  passed.

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
