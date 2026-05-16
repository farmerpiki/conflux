# HTTP server implementation module split proposal

Date: 2026-05-15  
Status: **recommended; implementation branch should avoid public API changes**  
Scope: split `src/net/http_server_impl.cxx`; explicitly excludes JSON modules

## Decision

Split `src/net/http_server_impl.cxx` next.

It is the largest non-JSON implementation unit in this snapshot at roughly 4.8k
lines, and the existing priority list already says to keep peeling
`http_server_impl` after the helper/config/router splits. The split is worth
implementing because it reduces review cost and incremental rebuild cost without
changing the public `conflux.net.http_server` surface.

Do not split `conflux.net.http_server` into new public package components in this
branch. Keep all new files private to the existing `conflux_http_server` target
until the send/recv/dispatch boundaries prove stable.

## Current shape

Already extracted:

```text
src/net/http_server.cxx             public API: HttpServer, RunStatus, metrics, send_zc state
src/net/http_server_helpers.cxx     response formatting + request parsing helpers
src/net/http_server_config.cxx      ring/config flag derivation
src/net/http_server_impl.cxx        ring state, TLS/H2, recv/send, CQE loop, dispatch, facade glue
```

Remaining mixed concerns in `http_server_impl.cxx`:

```text
connection/ring state and op packing
TLS handshake, BIO flush/feed, TLS send/shutdown paths
HTTP/2 nghttp2 callbacks, stream state, H2 SSE/deferred handling
io_uring init, accept, recv, send, close, timer, direct-slot management
recv buffer ownership: classic, recv-bundle, incremental buffer-ring paths
response send paths: plain, mapped file, streamed file, send_zc, TLS bypass
SSE/deferred response/WebSocket handoff state machines
CQE phase pipeline and fatal diagnostics
HTTP/1 request dispatch and response body classification
HttpServer facade: multi-ring setup, shutdown, metrics aggregation
```

## Proposed private module layout

Use internal/private module partitions or private module implementation units,
not public exported modules. The public module interface stays in
`src/net/http_server.cxx`.

Target shape:

```text
src/net/http_server_state.cxx
  module conflux.net.http_server:state;
  Op, pack/unpack, ServerFatalReason, PartialBuf, Conn, RecvComp, Ring declaration,
  low-level counters, shared constants.

src/net/http_server_tls.cxx
  module conflux.net.http_server:tls;
  TLS-only helpers and Ring methods under CONFLUX_HAS_TLS:
  tls_feed_rbio, tls_flush_wbio, tls_queue_send, queue_tls_shutdown,
  handle_tls_handshake/read/write/shutdown branches.

src/net/http_server_h2.cxx
  module conflux.net.http_server:h2;
  H2Stream/H2ConnCtx, nghttp2 callbacks, H2 response submission, H2 SSE/deferred
  integration. Entire file remains guarded by CONFLUX_HAS_HTTP2.

src/net/http_server_rx.cxx
  module conflux.net.http_server:rx;
  accept/recv arming, recv CQE normalization, classic/recv-bundle/incremental
  buffer ownership, phase1 recv-copy helpers.

src/net/http_server_tx.cxx
  module conflux.net.http_server:tx;
  plain send, send_zc, mapped-file writev, streamed-file splice/read-fixed,
  response completion and resubmission decisions.

src/net/http_server_realtime.cxx
  module conflux.net.http_server:realtime;
  deferred response wait/readiness, SSE polling, WS cancel/install/handoff paths.

src/net/http_server_loop.cxx
  module conflux.net.http_server:loop;
  pending-op drain, timers, shutdown, close paths, CQE phase pipeline,
  overflow/fatal diagnostics, run_loop().

src/net/http_server_dispatch.cxx
  module conflux.net.http_server:dispatch;
  HTTP/1 dispatch_request(), parse-error emission, handler timing diagnostic,
  response body classification into Conn state.

src/net/http_server_impl.cxx
  module conflux.net.http_server;
  imports private partitions; contains HttpServer::Impl, constructors/destructor,
  initialize(), request_shutdown(), shutdown(), run(), metrics(), port().
```

Expected end state: no private file exceeds roughly 1.2k lines; most files are
300-900 lines and map to one state machine.

## CMake shape

Keep a single library target and add private module sources. Sketch:

```cmake
add_library(conflux_http_server STATIC)
target_sources(conflux_http_server
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/http_server.cxx
)
target_sources(conflux_http_server
    PRIVATE FILE_SET private_http_server_partitions TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
            src/net/http_server_state.cxx
            src/net/http_server_tls.cxx
            src/net/http_server_h2.cxx
            src/net/http_server_rx.cxx
            src/net/http_server_tx.cxx
            src/net/http_server_realtime.cxx
            src/net/http_server_loop.cxx
            src/net/http_server_dispatch.cxx
)
target_sources(conflux_http_server
    PRIVATE src/net/http_server_impl.cxx
)
```

If private partitions are too brittle on a compiler lane, fall back to private
implementation units that keep `Ring` method definitions in fewer chunks. Do not
fall back to `.inc` textual includes unless compile-tooling support blocks real
module units; `.inc` files improve ergonomics but do not improve build memory or
incremental rebuild behavior.

## Dependency rules

Keep the internal graph acyclic:

```text
state
  <- tls
  <- h2
  <- rx
  <- tx
  <- realtime
  <- dispatch
  <- loop
  <- impl
```

Practical import rules:

```text
state       imports types, HTTP core/router/static/realtime/file/socket/uring vocabulary only as needed for fields.
tls         imports state + conflux.net.tls.
h2          imports state + conflux.net.http2.
rx          imports state + uring/socket/file_io buffer vocabulary.
tx          imports state + file_io + http_server_helpers + send_zc public helpers.
realtime    imports state + work + realtime/socket handoff vocabulary.
dispatch    imports state + http1_parser + http_server_helpers + router/vhost APIs.
loop        imports state + rx + tx + tls + h2 + realtime.
impl        imports loop + state + config/protocol/vhost facade dependencies.
```

Do not introduce a new public target dependency. Do not make HTTP/2, HTTP/3, TLS,
static, or realtime independently linkable from this branch.

## Implementation order

1. **State partition only.** Move `Op`, `pack/unpack`, `Conn`, `RecvComp`, shared
   constants, and the `Ring` declaration. Keep all method bodies in the old file.
   Compile after this step before moving behavior.
2. **Move dispatch.** Extract `ParseError`, `emit_parse_error()`, and
   `dispatch_request()`. This has clear boundaries and tests existing
   `http_server_helpers` ownership.
3. **Move protocol sidecars.** Extract TLS and HTTP/2 behind existing compile
   definitions. Keep disabled-feature builds green.
4. **Move RX/TX paths.** Split recv-buffer handling before send paths so response
   send code can still be compared against the old body.
5. **Move realtime handoffs.** Extract deferred/SSE/WS after RX/TX, because it
   touches both queues.
6. **Move loop/fatal diagnostics.** Move `run_loop()` last; it is the integration
   point and should remain easy to diff against the original until all helpers
   compile.
7. **Shrink facade.** Leave only `HttpServer::Impl` and public method definitions
   in `http_server_impl.cxx`.

## Ergonomics win

- Reviewers can reason about one state machine per file instead of scanning the
  entire HTTP server runtime.
- Public API stays stable while implementation churn happens behind one target.
- Future perf work gets narrower compile/test loops: send_zc changes hit TX;
  recv-bundle/incremental changes hit RX; TLS/H2 fixes hit sidecar files.
- The existing helper/config split becomes visible instead of being buried by the
  remaining monolith.

## Performance constraints

- Keep `Conn` and `Ring` concrete; no per-connection pimpl, virtual interface, or
  heap allocation layer.
- Keep `pack/unpack`, `PartialBuf`, and short state predicates inline in the
  state partition.
- Keep hot send/recv helpers as non-allocating functions or `Ring` methods. Do
  not introduce `std::function`/type-erased callbacks on the CQE path.
- Preserve current `Conn` field order unless a separate cache-layout patch proves
  a win with benchmarks.
- Preserve send_zc accounting semantics exactly; use existing send_zc tests and
  HTTP stress tests as the guard.
- Keep TLS/H2 compile definitions on the existing target so optional protocol
  builds do not fork behavior.

## Risk controls

- C++ module partitions can expose compiler fragility. Keep the public interface
  thin and run the module-fragility regression check after the state partition.
- Avoid cyclic partition imports by making `state` declaration-only and moving
  integration orchestration into `loop`.
- Move one concern at a time and keep pure code motion separate from behavior
  fixes.
- Do not combine this with response-body model redesign, router body decoupling,
  HTTP/3 changes, or JSON work.

## Validation checklist

```sh
scripts/check-cmake-source-files.py
scripts/check-module-interface-regressions.sh
cmake --preset dev
cmake --build --preset dev -j
ctest --test-dir build/dev --output-on-failure \
  -R 'http|h2|h3|tls|file_io_http|recv_bundle|send_zc|direct_accept|cq_overflow'
```

Run at least one narrow HTTP example after tests:

```sh
./build/dev/conflux_hello
./build/dev/conflux_static
```

## Not in scope

- No JSON module split.
- No new public package component.
- No public `HttpServer` API changes.
- No response body abstraction redesign.
- No P2300/work scheduler rewrite.
- No benchmark-driven `Conn` layout optimization in this branch.
