# Component Review TODO

Scope: `conflux.work`, `conflux.json`, `conflux.net.http_server` including `http1_parser`.

## Valid, deferred

- [x] JSON reader full-document API split
  - `decode(JsonReader&)` currently consumes one value and permits additional top-level values to remain for later reads.
  - Decide API shape: keep a clearly named streaming decode such as `decode_next<T>(JsonReader&)`, and add/rename a full-document decode that requires EOF after one value.
  - Files: `src/json.cxx`, `docs/json-api.md`, `tests/json_test.cxx`.

- [x] `JsonReader::skip_next_value()` API validation contract
  - Unknown-member decode no longer uses the structural unchecked skip on untrusted input, but the public `skip_next_value()` API itself remains documented as structural and non-validating.
  - Decide whether to rename it to make the unchecked behavior explicit or replace it with a validating skip built on reader events.
  - Files: `src/json.cxx`, `docs/json-api.md`, `tests/json_test.cxx`.

- [x] `join_all` cancellation semantics
  - Current `join_all` waits for all tasks and records the first failure; returned task cancellation is inert because the source disables cancellation, and sibling cancellation happens only after a child reports cancelled.
  - Decide whether to document this as wait-all semantics and/or add a separate fast-fail/cancel-propagating API such as `try_join_all`.
  - Files: `src/work.cxx`, `docs/conflux-work-root-api.md`, `tests/work_test.cxx`.

- [x] Blocking waits from WorkPool workers
  - A single-threaded `WorkPool` can deadlock if a worker job blocks waiting for another job enqueued to the same pool.
  - Decide whether to document "pool jobs must not synchronously wait on same-pool work", fail fast when detected, or add a helping/work-stealing wait mode.
  - Files: `src/work.cxx`, `docs/conflux-work-root-api.md`, `tests/work_test.cxx`.

- [x] Incremental chunked upload decoding
  - `decode_chunked()` is bounded but reparses from byte zero while waiting for a complete chunked request, making slow/chopped uploads O(n^2).
  - Add per-connection chunked decode state: current chunk size, remaining bytes, trailer count, decoded bytes.
  - File: `src/net/http_server.cxx`.

- [x] HTTP/1 `Expect: 100-continue`
  - Requests with `Expect: 100-continue` are parsed as normal requests today; clients that wait for the interim response before sending the body may stall until request timeout.
  - Add interim `100 Continue` support or reject unsupported expectations with `417 Expectation Failed`.
  - Files: `src/net/http_server.cxx`, `tests/http_e2e.cxx`.

- [x] TLS mapped-file streaming
  - TLS mapped-file responses still copy the mapped body into `own_response` before `SSL_write` when not using streamed files or kTLS.
  - Prefer streamed-file TLS chunks for large static files.
  - File: `src/net/http_server.cxx`.

- [x] WorkPool raw job exception policy
  - Worker loops intentionally swallow exceptions from raw `enqueue()` jobs; wrapped task jobs report errors through task state, but direct callers can lose failures.
  - Decide between an exception sink in `WorkPoolOptions`, a checked enqueue API, or documentation that raw jobs must not throw.
  - Files: `src/work.cxx`, `docs/conflux-work-root-api.md`.

- [x] Multi-error reporting for `join_all`
  - Current `join_all` preserves the first observed failure. This is coherent for a wait-all primitive, but callers cannot inspect additional failures.
  - Decide whether to document "first failure only" or add a separate aggregate-result API.
  - Files: `src/work.cxx`, `docs/conflux-work-root-api.md`.

- [ ] Borrowed JSON lifetime signaling
  - `parse_borrowed`, borrowed builder insertions, and NDJSON range entries are documented, but they still present an easy dangling-reference trap when callers mutate or release the backing buffer after parsing.
  - Decide whether to strengthen naming, examples, and API docs around "borrowed/unsafe/view" semantics.
  - Files: `src/json.cxx`, `docs/json-api.md`, `tests/json_test.cxx`.

- [x] JSON5 unterminated block comment diagnostic
  - The JSON5 comment skipper should report an explicit `unterminated block comment` instead of falling through to a generic EOF-style parse error.
  - Files: `src/json.cxx`, `tests/json_test.cxx`.

- [x] JSON object hash capacity arithmetic audit
  - Audit lazy object-index reserve/capacity math such as `count * 2` and convert any user-influenced multiplication to checked or division-form bounds.
  - File: `src/json.cxx`.

- [ ] WorkPool stop/destructor semantics
  - `stop()` is a hard stop that can abandon queued raw jobs; this is valid only with a loud contract, or separate drain-vs-shutdown APIs.
  - Files: `src/work.cxx`, `docs/conflux-work-root-api.md`, `tests/work_test.cxx`.

- [ ] Task second-consumer await behavior
  - `TaskAwaiter::await_suspend()` returns non-suspending when a ready callback is already installed, so accidental second awaits should be reviewed for deterministic failure rather than a possible blocking `await_resume()`.
  - Files: `src/work/root.cxx`, `tests/work_root_test.cxx`.

## Already tracked elsewhere

- [x] H2 pending send buffer cap
  - Already listed in `TODO_security_hardening.md`.

- [x] Multipart production parser hardening
  - Boundary-line parsing and multipart limits are tracked in `TODO_security_hardening.md`.

- [x] HTTP/2 request validation parity
  - Pseudo-header and header validation parity is tracked in `TODO_security_hardening.md`.

- [x] Chunked wire-overhead limits
  - Chunked raw receive and overhead policy is tracked in `TODO_security_hardening.md`.

- [x] `Expect: 100-continue` stress coverage
  - Additional provisional-response state tests are tracked in `TODO_security_hardening.md`.
