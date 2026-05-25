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
- `findings/6.md` P1 full route-pattern model unification and App/Router
  OpenAPI renderer unification are deferred as larger router/app metadata
  rewrites.
