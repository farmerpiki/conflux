# Findings implementation status

Status for the current findings audit. Existing finding files may already carry
local edits, so this file records the implementation state without rewriting
those reviews.

## Complete in current slice

- `findings/1.md`: accepted the common-header summary correctness issue.
  Fixed first-empty `Host`, `Content-Length`, `Content-Type`, and `Cookie`
  handling by tracking first-seen state separately from value emptiness.
- `findings/1.md`: accepted the request-buffer-pool lifetime issue.
  The HTTP/1 request buffer pool is now shared storage; recycled request buffers
  can outlive `Ring` when async/deferred request leases outlive the connection.
- `findings/4.md`: accepted the exact no-param route dispatch overhead issue.
  Exact path matches with no params, no route observation, and no HEAD remap now
  dispatch the original `RequestView` without rebuilding params/views.
- `findings/4.md`: accepted the eager context-route metadata allocation issue.
  Context task dispatch now builds route-pattern metadata only when route
  observation is enabled.
- `findings/7.md`: accepted the Content-Type prefix matching issue.
  Shared media-type helpers now strip parameters and compare case-insensitively;
  form, multipart, JSON-like compression, cache-control, and JSON helper paths
  use the shared parser.
- `findings/9.md`: accepted the header-source runtime SIMD mismatch.
  `HEADER_INTERFACE_WITH_SOURCES` plus `CONFLUX_SIMD_SELECTION=RUNTIME` now
  fails at configure time with an explicit diagnostic instead of implying
  unsupported runtime dispatch.
- `findings/9.md`: accepted the mock package smoke profile-wrapper mismatch.
  JSON header-mode package smoke no longer includes `<conflux/curated.hpp>`, so
  mock-liburing installs can keep profile wrappers forbidden while still
  validating the installed JSON component.
- `findings/9.md`: accepted the remaining JSON lifetime reset-edge coverage.
  `DocumentStorage::reset()` now destroys warmed object hash tables and clears
  external borrowed-pointer slots before reuse; `ValueBuilder::reset()` and
  `JsonArena` reuse paths use that helper.
- `findings/2.md`: accepted the fixed template operator table allocation
  cleanup. Compile-time and runtime comparison-operator descriptor tables now
  use `constexpr std::array` with `std::string_view` literals instead of
  first-use dynamic vectors.
- `findings/2.md`: accepted the client request-header overlay materialization
  cleanup. HTTP/1 client request building now constructs effective headers once
  as local `string_view` entries into existing request/default header storage,
  then reuses that vector for reserve sizing and serialization.
- `findings/7.md`: accepted the route-level App rate-limit eviction issue.
  Route-local rate limiting now uses the same LRU-bounded string map strategy as
  the middleware limiter instead of erasing an arbitrary unordered-map entry.
- `findings/5.md`: accepted the HTTP façade/package smoke purity issues.
  Package HTTP module smoke now imports `conflux.http` and exercises
  `http::app()`/`http::text()` instead of importing `conflux.net.http`; the
  façade import smoke no longer imports internal server types.
- Build verification: the libcurl external stress fixtures now disable direct
  accept so curl/TLS compatibility stress does not inherit fixed-file direct
  accept allocator nondeterminism. Direct accept remains covered by its own
  socket/recv-bundle tests.
- `findings/2.md`: accepted the public/internal header callback algorithm
  cleanup. `HttpFields` and `HttpFieldsView` now expose `for_each_value` and
  `any_value`, with free `for_each_header_value` / `any_header_value` wrappers,
  so duplicate header values can be scanned without allocating a vector.
- `findings/5.md`: accepted the API surface import smoke weakness. Curated,
  selected, extended, and complete import smokes now exercise representative
  public symbols instead of only importing their profile module.
- `findings/5.md`: accepted the CQ-overflow stress assertion weakness. The
  stress test now accepts normal stop or explicit CQ/submit pressure statuses,
  not a generic `fatal_internal_exception`.
- `findings/5.md`: accepted the SSE weak assertion gap. TLS, HTTP/2, and
  libcurl SSE tests now assert exact finite event-stream bodies instead of
  substring presence.
- `findings/5.md`: accepted the recv-bundle e2e weak assertion gap. The final
  liveness checks now validate the HTTP status line and exact JSON body instead
  of substring presence.
- `findings/5.md`: accepted the header compile-fail wiring/quality issue.
  Header-mode compile-fail tests are wired and verified; source fixes removed
  header-only masking errors around ambiguous `Request`, missing
  `<shared_mutex>`, and curated/extended exposure of `IoUringCaps`.
- `findings/1.md`: accepted the exported crypto detail namespace issue.
  `conflux.crypto` no longer exports `conflux::crypto::detail`; the
  allocation-free precomputed HMAC-SHA256 path is exposed as documented
  `conflux::crypto::HmacSha256Key`, `hmac_sha256_key`, and
  `hmac_sha256_precomputed` APIs for internal and advanced callers.
- `findings/2.md`: accepted the shared parameterized HTTP-list parser cleanup.
  `header_params` and `parse_http_q` now centralize semicolon-parameter and
  q-value scanning without materializing strings; compression negotiation,
  static precompressed negotiation, and response-cache Cache-Control parsing use
  the shared view parser.
- `findings/8.md`: accepted the DB pool off-owner lease capacity leak. Off-owner
  lease destruction now closes the physical connection and decrements the
  pool's total connection count atomically, so a later owner-thread acquire can
  create replacement capacity instead of treating the closed connection as still
  live.

## Accepted backlog

- `findings/2.md`: reflected JSON member lookup ergonomics/performance.
  Accepted direction, but not in this slice; needs a focused data-structure
  change and benchmarks.
- `findings/2.md`: client effective-header materialization, public/internal
  header callback algorithms, and shared parameterized HTTP-list parsing are
  complete above.
- `findings/2.md`: route-pattern/OpenAPI/template rendering deduplication.
  Accepted as design debt, but too broad for this safety/perf slice.
- `findings/3.md`: generic io_uring sync task driver, HTTP client body state
  machine, JSON parse ownership deduplication, and bench fixture deduplication.
  Accepted as larger refactors; not mixed into this slice.
- `findings/4.md`: per-request async lease allocation/copy avoidance on normal
  sync dispatch. Mostly addressed by the exact no-param fast path; broader
  lease-path tuning remains accepted backlog and should be benchmarked.
- `findings/4.md`: response gather-send, response storage normalization, and
  middleware allocation reduction. Accepted as performance backlog requiring
  benchmarks and isolated changes.
- `findings/5.md`: remaining structured-output assertion gaps stay accepted
  test backlog. HTTP package smoke, façade import purity, API surface symbol
  smokes, compile-fail wiring, CQ-overflow status tightening, SSE exact stream
  assertions, and recv-bundle exact response assertions are complete above.
- `findings/6.md`: first-contact HTTP ergonomics items such as required
  extractor semantics, form/query metadata split, typed responses, typed auth,
  SSE/WS polish, and quickstart spelling. Accepted as API design backlog.
- `findings/7.md`: JWT parser cleanup, request-derived parsing cache, chunked
  decoder/redirect/route-pattern/OpenAPI deduplication, and JSON escaping
  cleanup remain accepted backlog. Content-Type media-type parsing and
  route-level rate-limit eviction are complete above.
- `findings/8.md`: curated HTTP facade split, release-core target graph cleanup,
  direct DB cancel-pool exposure, SSE send-view semantics, header hygiene, and
  manifest cleanup. Accepted backlog; several are intentionally larger
  ownership/API changes. The DB pool off-owner capacity leak is complete above.
- `findings/9.md`: public header warnings remain accepted follow-up validation
  work; runtime SIMD configure behavior, mock package smoke behavior, JSON reset
  lifetime coverage, and crypto detail hiding are complete above.

## Rejected / not applied

- No reviewed finding in this pass was rejected on correctness grounds.
  Unimplemented items are deferred because they are larger refactors, API
  redesigns, or require benchmark-backed performance work rather than because
  the claim is considered invalid.

## Verification

- Final full `release-clang-libcxx` build completed after the libcurl stress
  fixture fix.
- Final full `ctest --test-dir /tmp/gcc-16/release-clang-libcxx
  --output-on-failure` completed: 1942/1942 passed.
- Targeted libcurl stress checks completed after the fixture fix:
  `ext/libcurl/stress: sequential requests` 1/1 passed and
  `ext/libcurl/stress: parallel multi-interface mixed routes` 1/1 passed.
- End-of-work re-evaluation of the explicit `HEADER_INTERFACE_WITH_SOURCES`
  plus runtime SIMD configure check fails with the intended diagnostic.
- `scripts/check-package-config.sh` completed.
- `scripts/check-package-smoke-liburing-free.sh` completed.
- `cmake --build --preset release-clang-libcxx --target conflux_json_tests`
  completed.
- `/tmp/gcc-16/release-clang-libcxx/tests/conflux_json_tests
  "[json][builder][lifetime],[phase5]"` completed.
- `cmake --build --preset release-clang-libcxx --target conflux_template_tests`
  completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "template:"` completed: 51/51 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_tests`
  completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "HttpFields zero-allocation value callbacks|HttpFields::values returns all
  entries"` completed: 2/2 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_api_surface_curated_import_smoke
  conflux_api_surface_extended_import_smoke
  conflux_api_surface_complete_import_smoke
  conflux_api_surface_selected_import_smoke` completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "api-surface/import-(curated|extended|complete|selected)"` completed: 4/4
  passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_http_overflow_stress_tests` completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http.cq_overflow"` completed: 1/1 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_tls_external
  conflux_h2_external conflux_libcurl_external` completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "SSE (streams all events|send_event delivers typed event|delivers all events
  over HTTP/2|short stream closes cleanly)"` completed: 5/5 passed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_recv_bundle_e2e_tests` completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "recv_bundle.e2e"` completed: 2/2 passed.
- Fresh header-interface compile-fail check completed:
  `cmake --preset release-clang-libcxx -B
  /tmp/gcc-16/header-compile-fail-check -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE
  -DCONFLUX_HEADER_INTERFACE_WITH_SOURCES=ON -DCONFLUX_BUILD_TESTS=ON
  -DCONFLUX_SIMD_SELECTION=DIRECT`, then `ctest --test-dir
  /tmp/gcc-16/header-compile-fail-check --output-on-failure -R
  "(http-facade-header/compile-fail|api-surface/header-)"`: 15/15 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_http_app
  conflux_uring conflux_pg` completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http client: request headers override default headers once|http client: GET
  /api/ping returns 200"` completed.
- `cmake --build --preset release-clang-libcxx --target
  conflux_http_facade_tests` completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http facade: route rate limit"` completed: 2/2 passed.
- `scripts/check-package-config.sh` completed after the HTTP package smoke
  change.
- `cmake --build --preset release-clang-libcxx --target
  conflux_http_facade_import_smoke` completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "http facade: public import smoke"` completed.
- Build-system re-evaluation after the latest build fixes found a generated
  header-surface regression where reduced API surfaces pulled in declarations
  requiring hidden `IoUringCaps`/`detect_caps` symbols. The fix keeps the
  curated/extended hiding contract and gates only cap-dependent transitive
  socket/runtime declarations in header mode.
- `ctest --test-dir build/release-clang-libcxx --output-on-failure -R
  "build/header-component-smoke"` completed: 1/1 passed.
- `cmake --build --preset release-clang-libcxx` completed after the final
  build fixes.
- `PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres ctest --test-dir
  build/release-clang-libcxx --output-on-failure` completed: 1940/1940 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_crypto
  conflux_http_auth conflux_crypto_tests conflux_password_hash_tests`
  completed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "api-surface/crypto-hides-detail|crypto:|password"` completed: 39/39 passed.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "build/header-component-smoke"` completed after the crypto API adjustment:
  1/1 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_http_core
  conflux_http_static_async conflux_http_compression conflux_http_policy
  conflux_tests conflux_compression_matrix_e2e conflux_file_io_http_e2e`
  completed after the shared HTTP parameter parser change.
- `ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R
  "response_cache:|cache_control:|static file|compression matrix|file_io http
  e2e|build/header-component-smoke"` completed after the shared HTTP parameter
  parser change: 49/49 passed.
- `cmake --build --preset release-clang-libcxx --target conflux_pg
  conflux_db_integration` completed after the DB pool off-owner capacity fix.
- `PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres ctest --test-dir
  /tmp/gcc-16/release-clang-libcxx --output-on-failure -R "db: pool"`
  completed after the DB pool fix: 5/5 passed.
- Fresh full `cmake --build --preset release-clang-libcxx` completed after
  the shared HTTP parameter parser and DB pool off-owner capacity fixes.
- Fresh full `PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres ctest
  --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure` completed:
  1945/1945 passed.
