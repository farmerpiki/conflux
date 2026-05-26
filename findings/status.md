# Findings status

Current review set: `findings/1.md` through `findings/10.md` after
`71aabad remove findings`.

## Complete

- `findings/8.md` non-hot algorithm/ranges cleanup batch: complete for the
  accepted low-risk items. `extension_allowed`,
  `has_pending_zc_notifications`, `mode_b_eligible`, JSON patch path prefix
  checks, template expression-list builders, process env/vector construction,
  vhost context-route detection, DB query-name validation, DNS section reads,
  DNS ASCII lowercasing, and DNS endpoint appends now use standard algorithms
  or bulk vector operations while preserving existing reserves and avoiding new
  copies.
- `findings/9.md` P0 drain/shutdown and large-body test oracles: complete.
  Shutdown and drain tests now require the listener to refuse post-stop
  connections instead of accepting would-block/no-quick-response as success.
  In-flight drain response checks now parse the header boundary and verify the
  complete response body length/content, including bytes read with the headers.
- `findings/9.md` P0 listener close behavior exposed by the stronger tests:
  complete. Shutdown and `drain(stop_accepting=true)` now cancel accept and
  close the listening socket/fixed-file slot, so new client connections are
  refused instead of being accepted and immediately idled/closed.
- `findings/9.md` P0/P1 weak test oracle cleanup: complete for H2 pump
  deadlines, compression negotiation bodies, facade JSON preconditions, static
  HEAD content length, and no-op `CHECK(true)` assertions. H2 pumps now fail on
  timeout, selected compression encodings are decoded and compared to the
  identity body, JSON helper preconditions use `REQUIRE`, the HEAD test matches
  an exact header line, and the probe-only tests use explicit `SUCCEED` or a
  real count invariant.
- `findings/10.md` P1 completion table free-list allocation: complete for the
  cheap bounded fix. `CompletionTable` now reserves `free_` alongside
  `slots_`, preventing the first dispatch/cancel free-list push from allocating
  when the table was constructed with an expected capacity.
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
- `findings/2.md` P0 JSON parse ownership/storage preparation: complete.
  Borrowed, copied, and moved input setup now share private storage-preparation
  helpers while preserving the explicit public overloads that document ownership.
- `findings/6.md` P1 route-level App rate-limit drift: complete for the
  accepted behavioral drift fix. Route-level rate limits now canonicalize IP
  keys with the same parse/format path as the middleware limiter; full store
  unification is deferred to avoid exposing middleware internals as public app
  API.
- `findings/6.md` P2 JSON extractor body parsing repetition: complete. JSON
  body content-type and max-body-size checks now share a private helper while
  leaving document/patch/merge-patch/typed decode branches explicit.
- `findings/2.md` P0/P1 HTTP rejection problem response construction:
  complete. HTTP/1 parser and timeout rejection paths now share
  `make_rejection_response(...)` and `note_rejection(...)` from
  `http_server_helpers`, leaving connection formatting and observability hooks
  local to each emitter.
- `findings/7.md` P2 and `findings/4.md` P2 build/API-surface
  maintainability scope: complete. Aggregate API profile exports now have a
  manifest checked against facade module sources, profile docs, and the
  component map. Package smoke can assert the installed API surface, the
  install-tree smoke forwards that expectation, and the package-config guard
  keeps those lanes wired. The package-smoke warning cleanup is also covered:
  mock-liburing package-mode notice is informational, and experimental
  import-std smoke support is opt-in instead of enabled for unrelated header
  package smokes.
- `findings/7.md` P1 public header hygiene coverage: complete. The hygiene
  checker now applies public shorthand-alias and macro leakage checks to every
  selected generated public header from the active manifest, while retaining the
  optional-dependency include guard for the core convenience header subset.
- `findings/4.md` P2 work-carrier single-failure checks: complete. The
  `when_all` single-failure tests now assert the original runtime-error message
  instead of accepting any `std::runtime_error`.
- `findings/1.md` P1 tiny route param type storage: complete. App route
  metadata now stores path parameter type tags in ordered flat vectors instead
  of tiny `std::map` node containers; lookup sites use linear `ranges::find`
  over the small bounded route parameter set.
- `findings/1.md` P0 route pattern parsing/storage: complete for the bounded
  grammar-drift issue. `router_match` now owns the shared parsed route-pattern
  result, including normalized shape, router segments, path parameter names,
  typed parameter tags, and validation errors; app route metadata consumes that
  parser instead of carrying a second grammar. The larger App/Router metadata
  model unification remains deferred with `findings/6.md`.
- `findings/4.md` P1 TLS/OpenSSL ALPN weak status checks: complete. Sequential
  `s_client` and ALPN fallback tests now assert the parsed HTTP status code is
  `200` in addition to checking the expected response body.
- `findings/4.md` P1 OpenAPI substring checks: complete. Served OpenAPI
  metadata tests and remaining auth/patch/merge OpenAPI checks now parse the
  spec JSON and assert fields by JSON pointer instead of relying on substrings.
- `findings/4.md` P1 API-surface positive smokes: complete. The extended
  import smoke now exercises representative HTTP extension, OpenAPI, offload,
  and work-root symbols; the complete import smoke now exercises representative
  uring, sync file I/O, socket I/O, and DNS symbols instead of only proving a
  single low-level `DirectFd` export.
- `findings/2.md` P1 JSON builder residual duplication: complete. Checked
  UTF-8 validation, duplicate-member error construction, member-name
  reservation, and object/array child setup now use private builder helpers
  while preserving the existing owned-name copy and borrowed-name pointer
  lifetime behavior.
- `findings/1.md` P2 compatibility-sensitive `views::zip` use: complete.
  JWT constant-time byte comparison and fixed-bucket metrics observation now
  use explicit indexed loops, making the constant-time path easier to audit and
  removing the only remaining `std::views::zip` dependency from the runtime
  sources.
- `findings/1.md` P1 header item range: complete. Added loop-based
  `header_items(...)` with shared quote-aware semicolon splitting and optional
  first-segment `name=value` support, then routed static Accept-Encoding,
  dynamic compression negotiation, and response-cache Cache-Control directive
  parsing through it.
- `findings/2.md` P1 work root join facade overloads: complete for the private
  state-check duplication. Public overloads remain explicit, while task,
  posted, operation, and join-handle `try_join_ready(...)` paths now share one
  checked ready-join helper for lifetime, capability, readiness, consume, and
  outcome extraction.
- `findings/1.md` P1 template top-level parser centralization: complete.
  Filter-pipe splitting now uses a callback top-level splitter, runtime
  `or`/`and` dispatch uses the shared top-level token finder, and method-call
  close-paren detection uses the shared matching-pair scanner.

## Verification

- `cmake --build --preset release-clang-libcxx --target
  conflux_template_tests conflux_json_tests conflux_process_tests
  conflux_tests conflux_work_tests conflux_db_tests conflux_dns_bridge`
  completed after the `findings/8.md` algorithm/ranges cleanup batch.
- `PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres ctest --test-dir
  /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "template:|json:.*patch|json: is_value|process:|db:|dns|uring|work\\.root|carrier\\."`
  completed after the `findings/8.md` batch: 320/320 passed, 1 skipped.
- `cmake --build --preset release-clang-libcxx --target conflux_tests
  conflux_http_full_drain_contract_e2e` completed after strengthening the
  drain/shutdown tests and closing the listener on stop/drain.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "shutdown\\(\\) stops run|drain stops new accepts|drain lets in-flight
  response finish|drain contract stops accepts"` completed: 4/4 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_h2_external
  conflux_compression_matrix_e2e conflux_http_facade_tests conflux_tests
  conflux_work_carrier_phase5_tests conflux_socket_task_ring_tests
  conflux_json_tests` completed after the remaining weak-oracle and completion
  table fixes.
- `PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres ctest --test-dir
  /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "h2:|compression matrix|http facade:.*json|HEAD / returns same
  headers|tcp_accept_multishot: listener destroyed|phase5c: TaskHandleAwaiter
  destroyed|JSONTestSuite: i_|shutdown\\(\\) stops run|drain stops new
  accepts|drain lets in-flight response finish|drain contract stops
  accepts|uring|work\\.root|carrier\\."` completed: 161/161 passed, 1 skipped.
- Full release verification after the `findings/8.md` through
  `findings/10.md` pass: `git diff --check` completed; `cmake --build
  --preset release-clang-libcxx` completed; `PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres
  ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure`
  completed: 1953/1953 passed, 7 skipped.
- Full release verification after the multipart parser benchmark/performance
  pass: `git diff --check` completed; `cmake --build --preset
  release-clang-libcxx` completed; `PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres
  ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure`
  completed: 1953/1953 passed, 7 skipped.
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
- `cmake --build --preset release-clang-libcxx --target conflux_json_tests`
  completed after centralizing JSON input preparation.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "json:"` completed: 204/204 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_http_facade_tests` completed after route rate-limit key
  normalization.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http facade: route rate limit"` completed: 3/3 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_http_facade_tests` completed after centralizing JSON extractor body
  gate checks.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http facade:.*json|http json:"` completed: 7/7 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_http_server
  conflux_tests conflux_http_server_helpers_tests` completed after sharing HTTP
  rejection response construction and metrics.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "parser: rejection metrics count classified HTTP/1
  rejects|body_timeout|header_timeout|http_server_helpers:"` completed: 15/15
  passed.
- `python3 scripts/check-public-header-hygiene.py --manifest
  /tmp/gcc-16/header-component-smoke/http-api/generated/bridge/module_header_bridge_manifest.json
  --include-dir /tmp/gcc-16/header-component-smoke/http-api/generated/bridge/include`
  completed: checked 133 public headers and 11 core dependency guards.
- `cmake --build /tmp/gcc-16/header-component-smoke/http-api --target
  conflux_header_smoke_public_hygiene` completed after regenerating the header
  bridge: checked 135 public headers and 11 core dependency guards.
- `cmake --build --preset release-clang-libcxx --target
  conflux_header_smoke_public_hygiene` was checked and is not a valid target in
  the release module profile; the header-component-smoke profile owns this
  header-only target.
- `cmake --build --preset release-clang-libcxx --target
  conflux_work_carrier_phase6_tests` completed after strengthening
  single-failure cause checks.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "phase6c: when_all single .* failure returns original cause unwrapped"`
  completed: 2/2 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_http_facade_tests` completed after flattening route parameter type
  metadata.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http facade: (fixed typed routes|typed route parameter tags dispatch)"`
  completed: 3/3 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_http_facade_tests conflux_tests conflux_h2_external` completed after
  moving route-pattern parsing into `router_match`.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http facade: (validate reports invalid route patterns|app groups support
  typed route patterns|app openapi spec maps typed path parameters|fixed typed
  routes|typed route parameter tags dispatch|validate reports positional path
  parameter mismatch|validate reports mismatched path extractor)|router:
  percent-encoded path param is URL-decoded|openapi: spec includes registered
  path with path parameter|h2: (POST body is echoed|GET with path param echoes
  name|GET /ping returns 200)"` completed: 13/13 passed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http facade: (app openapi spec maps typed path parameters|validate reports
  mismatched path extractor|validate reports positional path parameter
  mismatch|app openapi snapshot covers typed route policies|app openapi spec
  uses route metadata)"` completed: 5/5 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_tls_external`
  completed after strengthening TLS status assertions.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "ext/openssl: multiple sequential s_client connections all succeed|tls/alpn:
  (http/1.1 ALPN is accepted|unknown ALPN falls back without breaking h1)"`
  completed: 3/3 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_http_facade_tests` completed after strengthening OpenAPI JSON
  assertions.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http facade: (app openapi handler serves metadata spec|app openapi mounts
  metadata route|required basic auth extractor rejects missing credentials|JsonPatch
  extractor validates content type and patch shape|MergePatch extractor
  validates content type and body limit)"` completed: 5/5 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_api_surface_extended_import_smoke
  conflux_api_surface_complete_import_smoke` completed after expanding the API
  surface positive smokes.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "api-surface/import-(extended|complete)"` completed: 2/2 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_json_tests`
  completed after the JSON builder helper extraction.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "json:"` completed: 204/204 passed after the JSON builder helper extraction.
- `cmake --build --preset release-clang-libcxx --target conflux_jwt_tests
  conflux_tests` completed after replacing compatibility-sensitive
  `std::views::zip` loops.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "^(jwt:|metrics: Histogram|metrics: duration histogram appears in output)"`
  completed: 23/23 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_http_server_helpers_tests conflux_tests conflux_compression_matrix_e2e`
  completed after adding shared header item parsing.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http types: header_items|http_server_helpers: header parameter
  extraction|compress:|compression matrix|static precompressed|response_cache:
  Cache-Control"` completed: 22/22 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_work_root_tests conflux_work_carrier_tests conflux_work_api_snapshot`
  completed after the work-root ready-join helper extraction.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "work.root:"` completed: 58/58 passed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "carrier\\.model_a:|carrier\\.scope:|carrier\\.deadline:"` completed: 49/49
  passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_template_tests` completed after the template parser scanner cleanup.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "template:"` completed: 51/51 passed.
- Full release verification after the build-fix pass:
  `cmake --build --preset release-clang-libcxx` completed. Initial full CTest
  exposed four HTTP/1 rejection status-line regressions where timeout/Expect
  responses had correct numeric status and problem bodies but empty reason
  phrases. After adding the missing `408 Request Timeout` and
  `417 Expectation Failed` response reason mappings, targeted verification
  passed for the affected HTTP/1 Expect/slowloris tests and the response status
  table test. Final
  `PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres ctest --test-dir
  /tmp/gcc-16/release-clang-libcxx --output-on-failure` completed: 1953/1953
  passed, 7 skipped.
- Final verification after the route-pattern parser unification and status
  audit updates: `cmake --build --preset release-clang-libcxx` completed, then
  `PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres ctest --test-dir
  /tmp/gcc-16/release-clang-libcxx --output-on-failure` completed: 1953/1953
  passed, 7 skipped.

## Accepted / In Scope

- Non-performance correctness, lifetime-safety, copy-avoidance, and ergonomics
  claims in the current finding files remain in scope when they are not marked
  defer/do-not-touch/not-yet and do not require a massive rewrite.
- Performance work is now in scope only after those items are cleared or
  explicitly deferred, and only with representative benchmarks using the
  documented compare-bins/perf methodology.

## Completed Performance Work

- `findings/3.md` P0 default app `Json<T>` DOM-first decode: completed with
  representative app dispatch coverage. `src/net/app.cxx` now defaults typed
  owning `Json<T>` request bodies to direct decode while leaving
  `JsonDocument`, JSON Patch, Merge Patch, and explicit route-level decode
  options on the document/copy paths. `AppJsonOptions::direct_typed_decode`
  can disable the app-level direct default when copied input is needed.
  `benchmarks/http_app_path_bench.cxx` now includes `app_json_body`, which
  routes a POST JSON request through `App` and the `Json<T>` extractor rather
  than unit-testing the decoder in isolation.
- Evidence: compare-bins artifact directory
  `/tmp/gcc-16/bench-artifacts/20260525T183336Z-compare-bins`, base run
  `1027`, candidate run `1028`, selected with
  `BENCH_COMPARE_CONFIG_NAME=app_json_body`, pinned to CPU 2, 7 reps.
  Release-clang-libcxx wall time improved from base to candidate:
  best 1528.16 -> 1288.60 ns/iter (-239.56 ns, -15.68%),
  p10 1528.61 -> 1293.47 ns/iter (-235.14 ns, -15.38%),
  p50 1546.65 -> 1304.02 ns/iter (-15.69%),
  p99 1726.67 -> 1358.30 ns/iter (-21.33%). Allocation smoke at 10000
  iterations, preserved under
  `/tmp/gcc-16/bench-artifacts/20260525T-app-json-body-manual`, improved from
  30 allocations / 9156 bytes per request to 13 allocations / 1169 bytes per
  request.
- Sequential filtered `perf stat` for the same row is preserved in
  `base.seq.perf.json` and `candidate.seq.perf.json` under that artifact
  directory. Per request, instructions fell from about 20332 to 17971
  (-11.62%), cycles from about 7089 to 6497 (-8.35%), branches from about 4251
  to 3750 (-11.79%), and cache references from about 651 to 399 (-38.71%).
- `findings/3.md` P1 JSON parser string arena full-input reserve: completed.
  The parser no longer reserves the full input size before every parse. Object
  duplicate-key hashing stores stable member name offsets instead of
  `std::string_view`s into `string_arena`, so borrowed-string parses can keep
  avoiding copies without relying on arena pointer stability. Escaped/decoded
  string paths lazily reserve the old upper bound only when decoded storage is
  actually needed. `benchmarks/json_bench.cxx` now exposes compare-bins configs
  for `parse_large`, `parse_long_strings`, and `parse_escape_heavy`, and fixes
  the direct wide-object rows to respect `--filter`.
- Evidence: compare-bins artifacts
  `/tmp/gcc-16/bench-artifacts/20260525T194110Z-compare-bins`,
  `/tmp/gcc-16/bench-artifacts/20260525T194139Z-compare-bins`, and
  `/tmp/gcc-16/bench-artifacts/20260525T194207Z-compare-bins`, base/candidate
  runs `1071`/`1072`, `1073`/`1074`, and `1075`/`1076`: `parse_long_strings`
  best 53891 -> 32170 ns/iter (-21721 ns, -40.31%), p10 54682 -> 32671
  ns/iter (-22011 ns, -40.25%), p50 -40.25%, p99 -40.81%; `parse_large`
  best 961389 -> 952152 ns/iter (-9237 ns, -0.96%), p10 -7935 ns (-0.81%),
  p50 -0.81%, p99 -1.74%; `parse_escape_heavy` best 1145554 -> 472104
  ns/iter (-673450 ns, -58.79%), p10 -676255 ns (-58.80%), p50 -58.80%,
  p99 -58.69%.
- Isolated `perf stat` for `parse_long_strings` is preserved under
  `/tmp/gcc-16/bench-artifacts/20260525T-json-arena-lazy-reserve-perf`.
  Instructions fell from about 309.1M to 298.3M (-3.52%), L1-dcache load
  misses from about 2.73M to 2.19M (-19.51%), and dTLB load misses from about
  50.9k to 9.2k (-81.97%). Multiplexed cycles/branches/cache-reference counts
  rose in that perf-stat run, so the acceptance evidence is the repeated
  isolated compare-bins wall-time set plus the lower instructions/TLB/L1 miss
  counters.
- `findings/3.md` P0 middleware per-request function-wrapper allocation:
  completed for the bounded shared wrapper cost by adding inline storage to
  `CloneableFunction`. The one-shot `next` state still uses shared ownership
  for copied `next` handles, preserving current middleware semantics, but the
  per-request wrapper object no longer always needs a separate heap allocation.
  Allocation smoke improved `middleware_x4` from 27 allocations / 1648 bytes
  per request to 18 allocations / 1760 bytes per request, and `middleware_x16`
  from 77 allocations / 4160 bytes per request to 44 allocations / 4656 bytes
  per request. Allocation bytes rose because the remaining heap wrapper objects
  are larger, so acceptance is based on integrated wall-time and perf counters,
  not allocation count alone.
- Evidence: compare-bins artifact
  `/tmp/gcc-16/bench-artifacts/20260525T184535Z-compare-bins`, `middleware_x4`
  base run `1037`, candidate run `1038`: best 1112.90 -> 1095.55 ns/iter
  (-17.35 ns, -1.56%), p10 1115.16 -> 1096.46 ns/iter (-18.70 ns, -1.68%),
  p50 1120.72 -> 1100.02 ns/iter (-1.85%), p99 1132.41 -> 1121.01 ns/iter
  (-1.01%). Compare-bins artifact
  `/tmp/gcc-16/bench-artifacts/20260525T184548Z-compare-bins`, `middleware_x16`
  base run `1039`, candidate run `1040`: best 4074.84 -> 3904.51 ns/iter
  (-170.33 ns, -4.18%), p10 4075.31 -> 3907.24 ns/iter (-168.07 ns,
  -4.12%), p50 4110.84 -> 3927.85 ns/iter (-4.45%), p99 4155.76 -> 4000.51
  ns/iter (-3.74%). Filtered `perf stat` for `middleware_x16` is preserved
  under `/tmp/gcc-16/bench-artifacts/20260525T-cloneable-function-sbo-perf`;
  per request, instructions fell about 63674 -> 59233 (-6.97%), cycles about
  18856 -> 18273 (-3.09%), and branches about 13130 -> 11914 (-9.26%).
  L1/cache-reference counters worsened, so future broader middleware work
  should still inspect layout/cache effects.
- `findings/10.md` P1 multipart parser per-part header allocation/lowercase:
  completed with representative app-path benchmark coverage. The new
  `http_app_path` `multipart_mixed` row parses an HTTP/1 multipart/form-data
  request with text fields and file parts, dispatches through the router, and
  serializes the response. `parse_multipart(...)` now builds the boundary
  delimiter with reserve/append instead of `std::format`, and compares part
  header names case-insensitively as views instead of materializing a lowercased
  `std::string` for every header line.
- Evidence: compare-bins artifact
  `/tmp/gcc-16/bench-artifacts/20260526T041705Z-compare-bins`,
  `multipart_mixed` base run `1077`, candidate run `1078`, 7 reps pinned to
  CPU 2. Release-clang-libcxx wall time improved: best 2747.17 -> 2625.02
  ns/iter (-122.15 ns, -4.45%), p10 2765.27 -> 2631.94 ns/iter (-133.33 ns,
  -4.82%), p50 2779.70 -> 2669.97 ns/iter (-3.95%), p99 2796.09 -> 2712.24
  ns/iter (-3.00%).
- Filtered `perf stat` for the same row is preserved under
  `/tmp/gcc-16/bench-artifacts/20260526T-multipart-parser-candidate`. Per
  request, cycles fell about 12575.5 -> 12209.9 (-2.91%), instructions
  50631.3 -> 50092.5 (-1.06%), branches 10636.5 -> 10412.1 (-2.11%),
  branch misses 17.4 -> 15.1 (-13.04%), L1-dcache loads
  8508.8 -> 8000.7 (-5.97%), L1-dcache load misses 3.5 -> 2.6 (-27.18%),
  and dTLB misses also fell. Cache misses rose from about 1.3 to 1.5 per
  request in the single perf run, so future parser work should keep cache
  counters visible.

## Deferred / Perf-Gated

- `findings/8.md` JSON array/object equality algorithm rewrite is deferred
  until compile-time and representative JSON equality coverage are checked. It
  is recursive comparison code rather than a plain local loop, and the current
  pass kept to simple non-hot transformations with obvious allocation parity.
- `findings/8.md` duplicate `/etc/hosts` filtering helper is deferred as a
  larger DNS parsing cleanup. The accepted DNS changes removed repeated section
  reads/lowercasing/append loops; rewriting the address-family filtering shape
  should be done with resolver behavior tests focused on `/etc/hosts` edge
  cases.
- `findings/9.md` smuggling/close-path helper returning `{bytes, closed}` is
  deferred for a focused observability/connection-close contract pass. The
  current patch fixed the direct wrong-pass cases and strengthened local
  oracles, but changing raw read helpers across close-path tests is broader
  than this batch.
- `findings/9.md` socket/task lifetime no-op probes remain sanitizer/probe
  tests for now. The no-op `CHECK(true)` assertions were made explicit with
  `SUCCEED`, but stronger lifetime-state assertions require exposing or
  instrumenting internals that are not part of the current public contract.
- `findings/10.md` middleware-chain precomposition, dynamic route field-view
  overlays, observability/request-id request-local extension storage, common
  header-summary carry-forward, normal response split-send, default `Response`
  field slimming, JSON duplicate-key flat/PMR set, JSON whitespace SIMD,
  socket/DB intrusive async state, owned write buffer placement, and sink-based
  template rendering are deferred or perf-gated. They are hot-path or
  architecture changes and need representative compare-bins/perf evidence
  before acceptance; several also overlap with previously preserved rejected
  candidates.
- Remaining hot-path performance proposals from `findings/3.md` and
  performance-sensitive lazy/ranges changes from `findings/1.md` require
  representative benchmark coverage first. If no benchmark exists for a path,
  add one before evaluating a performance implementation.
- `findings/3.md` P0/P1 default `Response` status/content-type ownership:
  investigated but not accepted. A `ResponseString` static-or-owned field
  candidate is preserved as
  `/tmp/gcc-16/bench-artifacts/20260525T-response-string-perf/response-string-candidate.patch`.
  Integrated `http_app_path` allocation smoke did not improve (`get_ping`
  stayed at 6 allocations / 800 bytes per request; `json_small` stayed at
  5 allocations / 784 bytes per request). Compare-bins showed small p50 wins
  but weak/mixed tails: `get_ping` base run `1029`, candidate run `1030`,
  artifact `/tmp/gcc-16/bench-artifacts/20260525T183808Z-compare-bins`,
  p50 342.89 -> 334.69 ns/iter (-2.39%), p99 -0.71%; `json_small` base run
  `1031`, candidate run `1032`, artifact
  `/tmp/gcc-16/bench-artifacts/20260525T183842Z-compare-bins`, p50
  334.14 -> 325.01 ns/iter (-2.73%), p99 +2.09%. Filtered `perf stat` for
  `get_ping` under `/tmp/gcc-16/bench-artifacts/20260525T-response-string-perf`
  showed slightly lower cycles but higher instructions and cache activity, so
  the patch was reverted rather than kept.
- `findings/3.md` P0 HTTP/1 request dispatch double-parse/promote path:
  investigated but not accepted. A minimal parsed-view rebase candidate is
  preserved as
  `/tmp/gcc-16/bench-artifacts/20260525T-http1-dispatch-reparse-candidate/http1-dispatch-rebase-candidate.patch`.
  The candidate avoided the recursive `dispatch_request()` reparse after
  `PartialBuf::cut_prefix()` by rebasing `ParsedRequest` views into the promoted
  request buffer and zeroing `conn.request_bytes` because the prefix had already
  been removed. Correctness smoke passed for HTTP core/facade tests, but
  integrated HTTP server compare-bins did not clear the hot-path gate.
  `pipeline_100` compare-bins artifact
  `/tmp/gcc-16/bench-artifacts/20260525T190527Z-compare-bins`, base run `1045`,
  candidate run `1046`: best 906402.80 -> 907041.40 ns/iter (+638.60 ns,
  +0.07%), p10 907407.29 -> 908875.47 ns/iter (+1468.18 ns, +0.16%), p50
  911719.75 -> 916956.45 ns/iter (+0.57%), p99 920152.02 -> 943122.63 ns/iter
  (+2.50%). `post_echo_4k` compare-bins artifact
  `/tmp/gcc-16/bench-artifacts/20260525T190612Z-compare-bins`, base run `1047`,
  candidate run `1048`: best 19400.91 -> 18787.46 ns/iter (-613.45 ns,
  -3.16%), p10 19440.25 -> 19280.55 ns/iter (-159.70 ns, -0.82%), p50
  19983.99 -> 19752.56 ns/iter (-1.16%), but p99 regressed
  20964.36 -> 23601.87 ns/iter (+12.58%). The patch was reverted rather than
  kept; future work should look for a lower-overhead carry-forward of parsed
  offsets/header summary instead of rebasing/rebuilding the view set.
- `findings/3.md` P0 response body copied into formatted response string:
  investigated but not accepted for the lightweight plain-HTTP `writev`
  candidate. The patch is preserved as
  `/tmp/gcc-16/bench-artifacts/20260525T-response-body-split-candidate/response-body-writev-candidate.patch`.
  The candidate formatted only headers into `Conn::own_response`, moved the text
  body into connection-owned storage, and sent header/body with `writev` for
  normal non-TLS text responses. Correctness smoke passed for targeted HTTP
  tests, but integrated server benchmarking showed the extra send shape was
  slower than the current contiguous response buffer for the measured 4 KiB
  body path. Compare-bins artifact
  `/tmp/gcc-16/bench-artifacts/20260525T191652Z-compare-bins`, `post_echo_4k`
  base run `1049`, candidate run `1050`: best 16906.84 -> 19995.82 ns/iter
  (+3088.98 ns, +18.27%), p10 18078.52 -> 20062.24 ns/iter (+1983.72 ns,
  +10.97%), p50 19593.77 -> 20164.74 ns/iter (+2.91%), p99
  20144.01 -> 20543.16 ns/iter (+1.98%). The patch was reverted; future work
  should avoid replacing the single contiguous send with `writev` unless larger
  body thresholds or fixed-buffer interactions show a workload-specific win.
- `findings/3.md` P1 context exact route no-copy fast path: investigated but
  not accepted. Added representative `http_server` benchmark coverage for an
  exact async context route, then tested a narrow router fast path that skipped
  params/metadata rebuild while still passing an owned `RequestView` into the
  coroutine frame. The implementation candidate is preserved as
  `/tmp/gcc-16/bench-artifacts/20260525T-context-exact-fastpath-candidate/context-exact-fastpath-candidate.patch`.
  Correctness smoke passed for focused context/facade tests, but compare-bins
  did not clear the hot-path gate. Artifact
  `/tmp/gcc-16/bench-artifacts/20260525T192114Z-compare-bins`, `context_exact`
  base run `1051`, candidate run `1052`: best 21364.68 -> 19063.17 ns/iter
  (-2301.51 ns, -10.77%), p10 21708.95 -> 20373.92 ns/iter (-1335.03 ns,
  -6.15%), p50 22439.49 -> 22317.96 ns/iter (-0.54%), but p99
  23160.09 -> 26144.44 ns/iter (+12.89%). The router patch was reverted; the
  benchmark coverage remains for future context-route experiments.
- `findings/2.md` P0 generic io_uring sync-wait/task pump: deferred as a broad
  lifetime/cancel refactor rather than a bounded dedup patch. The repeated
  wait loops are exactly where cancellation and ownership can drift, but a safe
  shared pump needs a separate design pass over ring ownership, timeout CQEs,
  and blocking/task entry contracts.
- `findings/2.md` P0 HTTP client response body state: partially completed for
  redirect construction and chunked decoding. Remaining body/read-state
  unification is deferred until it can be designed with the io_uring/client
  pump work, because plain/TLS/sync/async ownership and body-limit behavior are
  coupled.
- `findings/2.md` P1 HTTP benchmark/server fixtures: deferred until the
  non-performance cleanup backlog is closed. It remains a candidate before any
  performance work so compare-bins/perf runs exercise realistic server/client
  paths rather than unit-level loops.
- `findings/2.md` P1 SIMD stdx/std26 backend algorithm deduplication:
  perf-gated. Do not centralize these hot algorithms until generated assembly
  and representative benchmarks prove the helper shape does not pessimize the
  selected backend.
- `findings/6.md` P1 JSON string escaping deduplication: perf-gated. The
  canonical dumper includes SIMD safe-run scanning, while direct/reflection
  writers are hot serialization paths. Centralizing them is reasonable only
  with representative JSON serialization benchmarks and codegen/perf evidence
  that the shared helper keeps or improves throughput.
- `findings/2.md` P1 App/Group route verb boilerplate: deferred until the
  route/extractor API model stabilizes. It is API-shape cleanup, and helper
  families now could hide rather than reduce the pending route model changes.
- `findings/2.md` P2 CMake target/module declaration helpers: deferred while
  header/module/profile/package behavior is still moving; a helper now would
  risk obscuring feature-guard edge cases.
- `findings/2.md` P2 HTTP server partition prelude and advanced example JSON
  diagnostics: rejected for current scope. The server prelude repetition is
  cosmetic and module-fragile, and example duplication is acceptable until the
  release examples/docs pass.
- `findings/2.md` P2 DB timeout/current-reader plumbing: rejected as
  standalone work. Only revisit if the DB deadline/cancel surface grows.
- `findings/7.md` P0 DB off-owner lease return waiter wakeup: deferred for a
  focused owner-thread marshalling design. The pool is owner-thread-affine and
  queued waiter state is not protected for off-owner mutation; the current
  minimum safe behavior closes the returned connection and releases capacity,
  but correctly waking waiters requires posting cleanup back to the owner rather
  than touching waiter queues from the destructor thread.
- `findings/7.md` P1 route timeout API consistency for async handlers: deferred
  for matched-route metadata propagation through the context dispatch chain.
  Applying timeout only after the task completes would not enforce the deferred
  deadline; the safe fix needs the matched route timeout available at
  `router_defer_http_task(...)`, including context-middleware paths.
- `findings/5.md` P0 curated HTTP surface tightening, typed status return
  redesign, body helper collapse, typed auth extractors, and middleware API
  unification are deferred as broad API/architecture changes rather than
  bounded correctness patches.
- `findings/5.md` P1 Query/Form typed extractor metadata, SSE/WebSocket route
  API unification, blocking-first client ergonomics, canonical run/listen
  vocabulary, human state/extractor diagnostics, and broader typed status
  helpers are deferred as ergonomic API-shape work. They should be revisited
  with the curated HTTP/Response split instead of patched piecemeal.
- `findings/5.md` P2 streaming uploads remain explicitly deferred; keep the
  bounded in-memory body contract until the public body/transport split is
  designed.
- `findings/6.md` P1 full route-pattern model unification and App/Router
  OpenAPI renderer unification are deferred as larger router/app metadata
  rewrites.
- `findings/1.md` P2 OpenAPI render-from-route-metadata-range is deferred with
  the App/Router OpenAPI renderer unification work. It is a tooling-path
  allocation cleanup, not a correctness or request-path issue, and the current
  materialization is simple until the public route metadata shape settles.
- `findings/1.md` P2 client effective-header indexing remains rejected for the
  current default-header size. Local materialization is lifetime-safe after the
  recent client fixes, and adding a normalized flat index only pays off if
  default or user-extensible header sets grow.
- `findings/7.md` P0 module-mode `release-core` target graph cleanup is
  deferred as a build graph/profile architecture split. Header-mode core is
  already checked; module-mode requires deciding whether `net.config`,
  `file_io_sync`, direct-slot-pool, and SIMD support are public core support or
  should move behind narrower profile targets.
- `findings/7.md` P1 standalone DB hidden singleton cancel pool is deferred for
  explicit API policy. Removing it or making it opt-in changes direct
  connection ergonomics and needs a public cancellation-worker story.
- `findings/7.md` P2 slim common `Response` object and docs/examples
  release-proof work remain deferred until after the curated HTTP/transport
  split and profile wrappers stabilize.
- `findings/4.md` P2 HTTP facade API snapshot internals remain deferred. The
  build-system part is covered by the checked API-surface manifest and package
  smoke API-surface assertion.
