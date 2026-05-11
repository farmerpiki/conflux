# Modular Build Targets & Feature Presets — updated review pass

Date: 2026-05-11  
Status: **recommended after boundary fixes**  
Scope: replacement patch-notes for the current modular-build proposal

## Decision delta

Keep the modular target direction. Before implementing the full preset resolver, add the missing file-adjacent boundaries and fix stale current dependencies.

The current proposal correctly identifies the monolith problem and the core/http split, but it is missing the sync file layer required by the no-stream and O_TMPFILE work. It also still assumes `http_static` depends directly on async `file_io`, which is only true for streamed file serving, not for metadata, mmap leases, or atomic sync writes.

## Immediate source-state fixes

Implement these before large CMake work:

```text
1. Remove PUBLIC conflux_file_io from conflux_socket_io.
   Current CMake links socket_io -> file_io even though socket sources do not need it.

2. Remove import conflux.file_io from src/net/client.cxx.
   It is stale and makes the client look file_io-dependent.

3. Remove or move HttpRequest::Builder::body_json(...).
   src/net/http_request.cxx imports conflux.json and body_json(NodeRef) is currently broken.

4. Make conflux_json always real.
   Reflection should depend on json; json should not be hidden inside the monolith.

5. Extract conflux_crypto early.
   JWT/HMAC/WebSocket masking/auth need crypto without TLS coupling.
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

conflux::http_router
  route matching, params, middleware chain.
  Depends: http_core.

conflux::http_server
  server loop, app dispatch, network runtime.
  Depends: http_router + runtime + socket_io.

conflux::http_static_core
  static path normalization, etag, range, precompressed metadata,
  mapped-body factories, sync atomic PUT helper.
  Depends: http_core + file_io_sync + file_map.

conflux::http_static_async
  streamed/splice file response integration.
  Depends: http_static_core + file_io.

conflux::http_realtime
  SSE + WebSocket factories for the current pre-v1 pass.
  Depends: http_server + crypto.
```

If splitting `http_static_core`/`http_static_async` is too much for the first patch, keep a single `http_static` target but be honest: it depends on `file_io` because streamed file serving is included. Do not describe it as sync/mmap-only.

## Preset changes

```text
core:
  BUILD_CORE=ON

runtime:
  core + BUILD_RUNTIME
  optional BUILD_SOCKET_IO=AUTO if preset wants network runtime
  no file_io

json:
  core + BUILD_JSON
  optional BUILD_JSON_FILE=OFF by default

http-minimal:
  runtime + socket_io + http_core + http_router + http_server
  no json, no static, no file_io

web-server:
  http-minimal + file_io_sync + file_map + file_io + http_static_async
  + template + compression + http_realtime + TLS/HTTP2 AUTO

http-api:
  http-minimal + json + http_json + crypto + http_policy + http_auth
```

Add component flags:

```cmake
CONFLUX_BUILD_FILE_IO_SYNC=AUTO
CONFLUX_BUILD_FILE_MAP=AUTO
CONFLUX_BUILD_JSON_FILE=OFF
CONFLUX_BUILD_HTTP_STATIC_CORE=AUTO
CONFLUX_BUILD_HTTP_STATIC_ASYNC=AUTO
```

## Core error prerequisite

Current async file I/O uses:

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

Do not make `file_io_sync` depend on `conflux_uring` just to reuse `IoError`.

## Response body split prerequisite

The current response variant names every optional body feature:

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

## Updated implementation order

```text
0. Core error and file_io_sync primitives.
1. Stale dependency cleanup: socket_io CMake, client import, broken body_json(NodeRef).
2. Always-real json and crypto targets.
3. O_TMPFILE temp-file sync + async publish redesign.
4. Stream cleanup in reusable sources.
5. file_map lease extraction.
6. Response body model split.
7. Router/static/realtime split.
8. Preset resolver and aggregate targets.
9. Tests/examples/benchmarks link smallest targets.
```

## Acceptance gates

```text
cmake -DCONFLUX_FEATURE_SET=core configures without liburing installed
cmake -DCONFLUX_FEATURE_SET=json configures without liburing installed
conflux_socket_io no longer links conflux_file_io
conflux_json is always a real target when BUILD_JSON=ON
json_file is optional and is the only JSON target using file_io_sync
static sync/mmap helpers do not import conflux.file_io
HTTP minimal target does not compile static serving, JSON, templates, OpenAPI, or compression
```
