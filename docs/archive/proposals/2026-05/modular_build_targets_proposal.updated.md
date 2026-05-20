> Archived historical rationale. Current branch selection lives in `todo/proposal_state.md` and `todo/parallel_priority_plan.md`.

# Modular Build Targets & Feature Presets — updated review pass

Date: 2026-05-11  
Status: **implemented component/preset graph; keep as historical target-boundary reference with noted remaining couplings**
Scope: replacement patch-notes for the modular-build proposal

## Decision delta

The modular target graph and preset resolver have landed. Keep this document as target-boundary rationale and source-state notes, not as an open implementation checklist.

The missing sync file layer and stale `http_static` dependency assumptions called out in this review have since been resolved: `file_io_sync` and `file_map` are standalone package components, `json_file` sits above `json + file_io_sync`, static HTTP is split into static surface/core/async targets, and `cmake/ConfluxPresets.cmake` resolves `CONFLUX_FEATURE_SET` plus `CONFLUX_BUILD_*` component flags.

## Immediate source-state fixes

Current source-state fixes already landed:

```text
1. conflux_socket_io no longer links conflux_file_io.
2. src/net/client.cxx no longer imports conflux.file_io.
3. HttpRequest::Builder no longer owns body_json(...) members; JSON request-body
   helpers live in conflux.net.http.json.
4. conflux_json is a real target when BUILD_JSON=ON; reflection depends on it.
5. conflux_crypto is a split target used by JWT/HMAC/WebSocket/auth surfaces.
6. Higher-level components/tests/benches no longer repeat direct
   PkgConfig::LIBURING links when conflux_uring/conflux_work/conflux_file_io/
   conflux_socket_io already propagate liburing usage requirements.
```

## Revised target graph

### Foundation / file layers

```text
conflux::core
  types, utils, generated features, core error type.
  No liburing.

conflux::file_io_sync
  POSIX-only fd/file helpers: UniqueFd, read/write/stat/fsync, O_TMPFILE temp files,
  atomic publish, line-oriented file loading adapters.
  Depends: core.
  No liburing, no work scheduler.

conflux::file_map
  read-only mmap lease and mapped file open helper.
  Depends: core + optionally file_io_sync for UniqueFd/stat helpers.
  No liburing.

conflux::runtime
  work scheduler + io_uring wrappers.
  Depends: core + liburing.

conflux::file_io
  async FileReader, splice, fixed buffers, registered files, async O_TMPFILE publishing.
  Depends: runtime + file_io_sync + file_map.

conflux::file_watch
  inotify/io_uring watch integration.
  Depends: file_io or runtime-specific watch target.
```

### JSON / templates

```text
conflux::json
  parser + DOM + SAX + NDJSON.
  Depends: core only.

conflux::json_file
  optional parse_file_sync/read_file convenience.
  Depends: json + file_io_sync.

conflux::json_reflect
  P2996 codec.
  Depends: json.

conflux::template
  string -> string engine with JSON data context.
  Depends: json + core + file_io_sync only for load_all convenience.

conflux::template_watch
  dev-mode hot reload.
  Depends: template + file_watch.
```

Do not let `conflux::json` import file I/O, mmap, logging globals, or io_uring.

### HTTP

```text
conflux::http_core
  HTTP types, request/response primitives, parser.
  Depends: core.

conflux::http_json
  request/response JSON helpers as free functions.
  Depends: http_core + json.

conflux::http_router_match
  route-pattern matching helpers.
  Depends: http_core + utils.

conflux::http_router_dispatch
  route dispatch helpers.
  Depends: http_core + http_response + http_realtime + work.

conflux::http_router
  public router surface and static/realtime compatibility exports.
  Depends today: http_core + http_response + http_realtime + http_static +
  work + utils + net_config + socket_io. This is a real split from the
  monolith, but not yet a pure http_core-only router target.

conflux::http_server
  server loop, app dispatch, network runtime.
  Depends: http_router + runtime + socket_io + related HTTP feature targets.

conflux::http_static_core
  static path normalization/cache internals.
  Depends: types only in current CMake.

conflux::http_static
  StaticOptions/static surface.
  Depends: types + work + net_config.

conflux::http_static_async
  static root-dir lifetime, contained open/probe helpers, GET/PUT/DELETE paths,
  and async file helper coroutines.
  Depends: http_static_core + http_static + http_response + file_io + file_map.

conflux::http_realtime
  SSE + WebSocket surfaces.
  Depends: http_core + crypto; links net_tls when TLS is enabled.
```

If splitting `http_static_core`/`http_static_async` is too much for the first patch, keep a single `http_static` target but be honest: it depends on `file_io` because streamed file serving is included. Do not describe it as sync/mmap-only.

## Preset changes

```text
current:
  transitional monolith-compatible default; most component flags resolve ON.

core:
  BUILD_CORE=ON only; no runtime/liburing.

runtime:
  runtime + socket_io; no file_io.

json:
  core + json; json_file remains OFF unless explicitly requested.

http-minimal:
  current source still resolves a richer server stack than the ideal minimal
  shape: runtime + file_io_sync + file_map + file_io + socket_io + dns + crypto
  + json + http_core + http_router + http_server + http_json. This is a known
  remaining coupling, not a documentation target to claim as already solved.

web-server:
  http-minimal + static + compression + realtime + template.

http-api:
  http-minimal + policy + auth.
```

Implemented component flags include `CONFLUX_BUILD_FILE_IO_SYNC`, `CONFLUX_BUILD_FILE_MAP`, `CONFLUX_BUILD_JSON_FILE`, and the current HTTP feature flags (`CONFLUX_BUILD_HTTP_STATIC`, `CONFLUX_BUILD_HTTP_REALTIME`, `CONFLUX_BUILD_HTTP_POLICY`, etc.). Static core/async are separate CMake targets under the current `HTTP_STATIC`/router stack rather than separate public `CONFLUX_BUILD_HTTP_STATIC_CORE` and `CONFLUX_BUILD_HTTP_STATIC_ASYNC` flags.

## Core error prerequisite

Resolved. `IoError` is exported from `conflux.types`, and `file_io_sync` no longer imports or links `conflux.uring.completion` / `conflux_uring`. Historical problem statement:

```cpp
export using FileIoError = IoError;
```

where `IoError` is defined in `conflux.uring.completion`. That prevents a no-liburing `file_io_sync` from returning the same error type.

Choose one:

```text
Preferred:
  move IoError to conflux::core as a generic system_error wrapper
  uring imports/reuses it
  file_io_sync imports/reuses it

Acceptable:
  define FileError in file_io_sync
  async file_io converts FileError <-> IoError at boundaries
```

Implemented via the preferred direction: the shared error vocabulary lives at the core/types layer, and `file_io_sync` remains no-liburing.

## Response body split prerequisite

Partially resolved. `conflux::http_response`, `conflux::http_static(_core/_async)`, and `conflux::http_realtime` are separate targets, but the current response/router stack still has concrete dependencies on file/realtime/static response surfaces. Historical problem statement:

```cpp
variant<S, SP<SseChannel>, SP<WsUpgrade>, SP<MappedFile>, SP<StreamedFile>, SP<DeferredResponse>>
```

Router cannot become `http_core`-only until that is fixed.

Recommended order:

```text
Phase 2a: body model decoupling
  - core owns bytes/text, status, headers, cookies, trailers
  - extension modules expose factories that attach body boxes/ops
  - server dispatch uses BodyKind + opaque ops, not concrete SSE/WS/static types

Phase 2b: router split
  - router_core
  - http_static_core/http_static_async
  - http_realtime
```

## Updated implementation status

```text
[x] Core error and file_io_sync primitives.
[x] Stale dependency cleanup: socket_io CMake, client import, broken body_json(NodeRef).
[x] Always-real json and crypto targets.
[x] O_TMPFILE temp-file sync + async publish redesign.
[x] Stream cleanup in reusable sources.
[x] file_map lease extraction.
[x] Router/static/realtime target split.
[x] Preset resolver and aggregate/component flags.
[~] Response/body/router decoupling: split targets exist, but router/response still carry static/realtime/file couplings.
[~] Tests/examples/benchmarks mostly use component targets; keep tightening when touching each binary.
```

## Acceptance gates

```text
cmake -DCONFLUX_FEATURE_SET=core configures without liburing installed
cmake -DCONFLUX_FEATURE_SET=json configures without liburing installed
conflux_socket_io no longer links conflux_file_io
conflux_json is always a real target when BUILD_JSON=ON
json_file is optional and is the only JSON target using file_io_sync
static sync/mmap helpers do not import conflux.file_io
HTTP minimal target remains richer than the ideal no-JSON/no-file-IO shape until router/response/server couplings are reduced
```
