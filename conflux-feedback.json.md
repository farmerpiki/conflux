# Conflux Feedback (From Migrating `bettergrok`)

## `conflux.json` complaints

1. Missing key vs explicit `null` is hard to distinguish in read paths.
- `operator[]` returns a null `Value` both for missing keys and real JSON null.
- This makes validation logic verbose and fragile.

2. Object lookup is linear-time (`vector<pair<string, Value>>`).
- Repeated key lookups are O(n) and become expensive in hot paths.
- For API payload parsing this is measurably awkward and potentially slow.

3. No ergonomic typed decode layer.
- Every parse requires manual field extraction and conversion.
- No built-in mapping to structs, no schema-like decode helpers.

4. Path access is too manual.
- No safe helper like `get_path("a.b[0].c")`.
- Deep extraction requires repetitive null/type checks.

5. Builder API is better than Boost raw, but still noisy for protocol payloads.
- Constructing nested request bodies requires lots of `set(...)` + local temporaries.
- Lacks compact object/array literals that keep static type safety.

6. Error model is parse-only.
- Parse errors are good, but runtime conversion/access errors are mostly ad-hoc.
- No cohesive diagnostic type for "missing field / wrong type / invalid domain value".

7. Duplicate-key policy is not explicit enough.
- Behavior with duplicate object keys in parsed input should be documented and controllable.

8. Numeric handling ergonomics are limited.
- Caller must manually probe `int64_t`/`uint64_t`/`double` in many places.
- No convenient `get_number_as<T>()` with checked coercion policy.

9. Interop friction during migration.
- Bridging from other JSON ecosystems is mostly serialize/parse round-trips.
- No direct adapters from common JSON types or parser events.

10. Missing utility helpers for common API patterns.
- No built-in helpers like `require_string(obj, "field")`, `optional_bool(...)`, etc.
- Leads to repetitive, easy-to-get-wrong boilerplate.

11. Const/read API can still encourage mutation patterns accidentally.
- Mixing mutable and immutable accessors with `operator[]` is easy to misuse.
- A clearer strict-read API would reduce mistakes.

12. No canonical pretty-print controls beyond `dump()` default.
- Useful for logs/debugging to have deterministic pretty output options.

13. No incremental/streaming parse interface.
- Parsing SSE-like or chunked JSON streams requires custom buffering glue.

14. Large-document ergonomics are weak.
- No explicit view/borrow traversal API optimized for repeated reads.

15. API discoverability is low.
- Important best practices (borrowed vs owned, null/missing handling) are not obvious from signatures alone.

## `conflux` HTTP client complaints

1. Synchronous-only request API in the primary path.
- For interactive apps this forces thread offloading externally.
- A first-class async/cancellable API should exist in the client module itself.

2. No streaming response callback API.
- Cannot consume body chunks as they arrive.
- This blocks robust SSE support and progressive rendering.

3. Cancellation is external and coarse.
- No request-level cancellation token integrated into the API.
- Callers must rely on timeout or kill the worker thread context.

4. Timeout model is too coarse.
- Single timeout value for connect/write/read phases is limiting.
- Need phase-specific and total deadline controls.

5. Error type is `expected<..., string>`.
- String errors are hard to classify programmatically.
- Need structured error enum/category + richer context.

6. No explicit retry policy hooks.
- Callers reimplement retry logic repeatedly.
- Built-in policy hooks would reduce duplicated, inconsistent behavior.

7. Limited transport observability.
- Missing exposed timing breakdown (dns/connect/tls/ttfb/body).
- Hard to debug latency and failures in production.

8. No connection pooling/reuse abstraction at API level.
- Typical API-client workloads need persistent pooled connections.
- Current call style encourages connect-per-request patterns.

9. SSE ergonomics are poor.
- No dedicated SSE client parser/iterator built into HTTP client module.
- Every app reimplements line framing and event parsing.

10. Backpressure controls are absent.
- No obvious way to control max buffered body/chunk strategy while streaming.

11. Limited request body streaming options.
- Current API is mostly full-buffer body string oriented.
- Large upload flows need chunked/streamed request-body support.

12. HTTP protocol-level capabilities are not surfaced clearly.
- Hard to know/control h1/h2/h3 client behavior from the high-level API.

13. Redirect and cookie policies are caller burden.
- No standard optional policy controls (follow redirects, cookie jar, limits).

14. Proxy support is not first-class in the client API.
- Enterprise/network-restricted environments need this.

15. TLS diagnostics are thin for callers.
- No structured exposure for cert/hostname/verification failure reasons.

16. Compression/content decoding behavior is not explicit enough.
- Need clear, controllable auto-decompression policy and metadata.

17. Header ergonomics are mixed.
- Better than raw sockets, but still verbose for common auth/content patterns.

18. Host/path URL ergonomics are awkward.
- Caller must split and manage host/port/path repeatedly in many apps.
- A robust URL request entrypoint should be available.

19. Missing middleware-like interceptors on client side.
- No standard request/response hooks for auth refresh, tracing, retries, metrics.

20. Testing/mocking story is weak at API boundaries.
- Need easy injection points for fake transports without rewriting call sites.

## `conflux.work` complaints

1. Cancellation semantics are not first-class at task API boundaries.
- Work submission/execution lacks a built-in cancellable operation handle.
- Interactive CLIs need direct, request-scoped cancellation, not only cooperative polling.

2. Interrupt integration is left to the caller.
- Mapping signals (like Ctrl+C) into work cancellation requires custom glue.
- This leads to duplicated patterns and inconsistent behavior across apps.

3. Completion polling ergonomics are basic.
- Typical flow ends up as repeated `sleep/poll` loops around futures/flags.
- A richer wait API with wake conditions would reduce boilerplate.

4. No standardized progress/event channel for long-running jobs.
- Callers must invent side channels for status updates.
- This is painful for TUI/GUI apps that need responsive intermediate feedback.

5. Error propagation conventions are too ad-hoc.
- Different call sites return different shapes (`bool`, `string`, `expected`, exceptions).
- A consistent async error model would make integration safer.

6. Task lifecycle introspection is limited.
- No obvious built-in state query for queued/running/completed/cancelled/failed.
- Debugging hangs and shutdown races becomes guesswork.

7. Shutdown/drain behavior is not explicit enough.
- It is unclear how queued jobs are handled on pool teardown in all cases.
- Predictable stop/drain policies should be configurable and documented.

8. Deadline and timeout primitives are too manual.
- Callers frequently implement their own deadlines with clocks and loops.
- Native deadline-aware APIs would reduce bugs.

9. Composition utilities are minimal.
- Common async patterns (`when_any`, `when_all`, race, timeout wrapper) are not ergonomic.
- Callers end up building bespoke combinators repeatedly.

10. Thread-affinity/executor selection is not surfaced clearly.
- Some tasks need main-thread callback handoff or specific execution contexts.
- Without clear executor routing, UI integrations become fragile.

11. Backpressure and queue sizing controls are not prominent.
- It is easy to enqueue unbounded work under load by accident.
- Explicit bounded queues + rejection policies should be first-class.

12. Instrumentation hooks are limited.
- There is no obvious standard hook set for queue depth, run time, wait time, and failures.
- Operational visibility matters for async-heavy applications.

13. Testing determinism is hard.
- Reproducible unit tests need deterministic scheduling helpers.
- Without test-oriented executors/fake clocks, async tests are flaky or slow.

14. Documentation around cancellation and memory visibility could be stronger.
- Async correctness depends on clear happens-before and ownership rules.
- Practical cookbook guidance for common patterns would help a lot.

15. API discoverability is uneven.
- Core patterns are possible, but the intended "golden path" for app developers is not obvious.
- Better high-level examples would lower adoption friction.
