# Modular Build Targets & Feature Presets

> State note (2026-05-16): superseded by
> `proposals/modular_build_targets_proposal.updated.md` and
> `todo/proposal_state.md`. The component/preset graph has landed; this file is
> retained as historical design rationale, not an open implementation checklist.

Date: 2026-05-11
Status: PROPOSAL
Effort: phased, 3–5 weeks across normal dev
Prerequisite: none (incremental, backwards-compatible at each step)

## Problem

`conflux` target is monolithic. It compiles JSON, templates, file watching,
crypto, HTTP server/client, router, auth, compression, CORS, rate limit,
static serving, OpenAPI, proxy, etc. into one library. A user who needs
JSON parsing pays for io_uring. A minimal HTTP server pays for OpenAPI and
template engine BMIs. This conflicts with stated design goals: optional
targets, compiled modules, no bundled JSON lock-in, additive advanced features.

Current `conflux.cxx` re-exports everything:

```cpp
export import conflux.types;
export import conflux.file_watch;
export import conflux.templates;
export import conflux.json;
export import conflux.net.http;   // -> 35+ middleware/feature modules
```

`conflux.net.http` re-exports auth, compress, cors, rate_limit, security,
forwarded, request_id, ip_filter, cache_control, trailing_slash, jwt,
metrics, http2, redirect, cookie_signing, csrf, etag, response_cache,
structured_log, tracing, vhost, openapi, client, etc.

### What already works

Low-level targets exist and are correctly split:

```
conflux_types, conflux_uring, conflux_work, conflux_file_io,
conflux_socket_io, conflux_dns, conflux_db
```

The gap is the top: `conflux` itself and every HTTP-adjacent module.

## Naming Convention

Replace mixed `CONFLUX_ENABLE_*` with three-part scheme:

```cmake
CONFLUX_FEATURE_SET=<preset>        # high-level bundle
CONFLUX_BUILD_<component>=AUTO|ON|OFF   # compile/link this component
CONFLUX_WITH_<backend>=AUTO|ON|OFF      # external dep/impl backend
CONFLUX_TUNE_<knob>=value           # perf/runtime tuning
```

`ENABLE` is ambiguous — compile-time? runtime? dependency? `BUILD` vs `WITH`
makes intent obvious. `AUTO` means "ON if preset wants it and deps available."

Keep io_uring runtime knobs (ring sizes, submission batching, registered
buffers) as runtime config, not CMake flags. They're kernel/environment
dependent.

## Presets

| Preset | What you get |
|--------|-------------|
| `current` | Today's behavior. Default during transition. |
| `core` | Types, errors, utils. No liburing, no HTTP, no JSON. |
| `runtime` | Core + work scheduler + io_uring wrappers. |
| `json` | Core + full JSON (parser/DOM/SAX/NDJSON). No runtime needed. |
| `http-minimal` | Runtime + sockets + HTTP/1 parser + router + server. No TLS, JSON, static, auth, compression. |
| `http-api` | http-minimal + JSON binding + policy (CORS/forwarded/IP filter) + auth. |
| `http-api-full` | http-api + observability + OpenAPI. |
| `web-server` | http-minimal + TLS + HTTP/2 + compression + static files + templates + SSE/WebSocket. |
| `http-server-complete` | http-api + web-server + client + proxy. |
| `complete` | Everything buildable with available deps. |

After pre-v1 cleanup, default switches from `current` to `http-api` or
`http-minimal`.

**Q: Should `runtime` preset include file_io + socket_io?** The review argues
no — "building own protocol runtime" shouldn't imply file I/O. I lean toward
including socket_io (runtime without sockets is useless for most people) but
*not* file_io. Users doing pure network protocols don't need file I/O.
Counter-argument: socket_io currently depends on file_io via shared uring
submission paths. Need to verify if that dependency is real or incidental.

## Public CMake Targets

### Foundation

```
conflux::core            types, utils, generated features. No liburing.
conflux::runtime         work scheduler, uring wrappers. Depends: liburing.
conflux::file_io         file I/O. Depends: runtime.
conflux::file_watch      inotify/io_uring watch. Depends: file_io.
conflux::socket_io       sockets, coro. Depends: runtime.
conflux::dns             DNS resolution. Depends: socket_io.
conflux::process         child process mgmt. Depends: runtime.
conflux::crypto          AES-NI, hashing. Independent of TLS.
```

### JSON

```
conflux::json            parser + DOM + SAX + NDJSON. Depends: core only.
conflux::json_reflect    P2996 reflection codec. Depends: json. GCC 16+.
```

**Q: Should JSON be further split (json_core/json_dom/json_stream/json_ndjson)?**
Review suggests yes. I think not yet — current JSON module is one TU, internal
layering is clean, and nobody will link json_dom without json_core. Premature
split adds BMI overhead with no real user benefit. Revisit if JSON grows large
enough that SAX-only users measurably pay for DOM codegen.

### Templates

```
conflux::template        template engine. Depends: json + core.
conflux::template_watch  hot-reload. Depends: template + file_watch.
```

This is a key fix: production template rendering should not pull file_watch →
file_io → liburing. Current `src/template.cxx` imports `conflux.file_watch`
directly. Need to split: template engine does string→string transform with
JSON data, file_watch integration is separate.

### HTTP

```
conflux::http_core       http.types, request/response primitives, HTTP/1 parser.
conflux::http_router     route matching, groups, middleware chain. Depends: http_core.
conflux::http_server     server loop, app, config. Depends: http_router + runtime + socket_io.
conflux::http_client_sync  blocking POSIX client. Depends: http_core + utils + optional TLS.
conflux::http_client_async async client. Depends: http_client_sync + socket_io + dns + cancel.
conflux::http_client     aggregate (async by default).
conflux::http_json       request/response JSON helpers. Depends: http_core + json.
conflux::http_static     static files, file cache, etag, range, precompressed, splice. Depends: file_io.
conflux::http_sse        SSE channels. Depends: http_server.
conflux::http_websocket  WS frames, connections. Depends: http_server + crypto.
conflux::http_realtime   Meta: http_sse + http_websocket.
conflux::http_policy     CORS, forwarded, ip_filter, security headers, rate_limit.
conflux::http_auth       auth, cookie signing, CSRF, JWT. Depends: crypto.
conflux::http_compression  gzip/brotli/zstd. Depends: respective WITH_ backends.
conflux::http_observability  metrics, structured_log, tracing, request_id.
conflux::http_openapi    OpenAPI generation/docs.
conflux::http_vhost      virtual host dispatch. Depends: http_router + work.
conflux::http_proxy      reverse proxy. Depends: http_client.
```

**Q: Is splitting SSE from WebSocket worth it?** WebSocket needs crypto (for
masking), SSE doesn't. But SSE is tiny — maybe 200 lines. Separate targets
means separate BMIs, separate link steps. Lean toward keeping `http_realtime`
as one target with the crypto dep, unless someone profiles and shows the
crypto BMI cost matters for SSE-only users.

### Aggregates (meta-targets, link-only)

```
conflux::http_minimal          http_server (implies http_core + http_router + runtime + socket_io)
conflux::http_api              http_minimal + json + http_json + http_policy + http_auth
conflux::http_api_full         http_api + http_observability + http_openapi
conflux::web_server            http_minimal + file_io + http_static + template + http_compression + http_realtime + TLS + HTTP/2
conflux::http_server_complete  http_api + web_server + http_client + http_proxy
conflux::complete              everything built
```

### Services

```
conflux::db_postgres     rename from conflux_db. Keeps alias.
conflux::smtp            depends: socket_io + crypto.
```

## Critical Refactors (ordered by dependency)

### 1. Conditional liburing discovery

Current `Dependencies.cmake`:

```cmake
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)
```

Must become conditional. `core` and `json` presets should configure without
liburing installed.

```cmake
if(CONFLUX_NEEDS_RUNTIME)
    pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)
endif()
```

Where `CONFLUX_NEEDS_RUNTIME` is resolved from preset + explicit BUILD flags.

### 2. Move JSON out of http_request

`src/net/http_request.cxx` imports `conflux.json` for `body_json()` helpers.
This means any HTTP user pays for JSON BMI. Move to new
`src/net/http_json.cxx` as extension functions/free functions that take
HttpRequest::Builder&.

Before: `request.body_json(doc)` (member fn)
After: `conflux::http::json::body(request, doc)` or keep as member but in
separate TU via extension module that augments the builder.

**Q: C++ modules don't support reopening classes across TUs. So `body_json`
can't stay as a member function in a different module.** Options:
  - (a) Free functions: `http_json::set_body(builder, doc)` — clean but API change
  - (b) Keep in http_request but make json an optional import via `if consteval`
        or feature flag — ugly, defeats purpose
  - (c) Keep body_json in http_core, accept json dep — gives up the split

I favor (a). It's the honest API: if you want JSON in HTTP, link http_json.

### 3. Split router

Current `src/net/router.cxx` imports:

```
conflux.crypto, conflux.work, conflux.file_io, conflux.socket_io,
conflux.net.config, conflux.net.tls
```

This is the biggest coupling problem. Router contains route matching AND
static file serving AND SSE AND WebSocket AND upload helpers AND mapped files.
Must split into:

```
router_core.cxx    → route matching, param extraction, groups, middleware chain
http_static.cxx    → serve_static, file cache, etag, range, precompressed (already partially exists?)
http_sse.cxx       → SSE channel creation/sending
http_websocket.cxx → WS upgrade, framing, connection
```

Router core should depend only on http_core + types. No crypto, no file_io,
no tls.

**This is the hardest refactor.** Router is ~3070 lines with interleaved
concerns. Splitting while keeping API compatibility requires careful interface
design at module boundaries.

### 4. Split template from file_watch

`src/template.cxx` imports `conflux.file_watch`. Template engine core is pure
string→string with JSON data context. File watching is for dev-mode hot reload.

Split: template core (json dep only) + template_watch (adds file_watch).

### 5. Rename DB target

`conflux_db` → `conflux_db_postgres`, `CONFLUX_ENABLE_DB` → `CONFLUX_BUILD_DB_POSTGRES`.
Keep old names as aliases during transition.

### 6. Decouple JWT from TLS

Current CMake only builds jwt.cxx when `CONFLUX_HAS_TLS`. But jwt.cxx imports
`conflux.crypto`, not `conflux.net.tls`. HMAC-SHA256 JWT (HS256) needs only
crypto, not OpenSSL. Only RS256/ES256 need OpenSSL for asymmetric keys.

Gate: `CONFLUX_BUILD_HTTP_AUTH` builds auth+cookie+csrf+jwt-hmac always.
`CONFLUX_WITH_TLS_OPENSSL` enables asymmetric JWT backends additionally.

## Concrete Flags

### Component flags (BUILD)

```cmake
CONFLUX_BUILD_CORE=ON                      # always
CONFLUX_BUILD_RUNTIME=AUTO
CONFLUX_BUILD_FILE_IO=AUTO
CONFLUX_BUILD_FILE_WATCH=OFF
CONFLUX_BUILD_SOCKET_IO=AUTO
CONFLUX_BUILD_DNS=AUTO
CONFLUX_BUILD_PROCESS=OFF
CONFLUX_BUILD_CRYPTO=AUTO

CONFLUX_BUILD_JSON=AUTO
CONFLUX_BUILD_JSON_REFLECT=OFF

CONFLUX_BUILD_TEMPLATES=OFF
CONFLUX_BUILD_TEMPLATES_WATCH=OFF

CONFLUX_BUILD_HTTP_CORE=AUTO
CONFLUX_BUILD_HTTP_ROUTER=AUTO
CONFLUX_BUILD_HTTP_SERVER=AUTO
CONFLUX_BUILD_HTTP_CLIENT=OFF
CONFLUX_BUILD_HTTP_JSON=AUTO
CONFLUX_BUILD_HTTP_STATIC=OFF
CONFLUX_BUILD_HTTP_REALTIME=OFF
CONFLUX_BUILD_HTTP_POLICY=AUTO
CONFLUX_BUILD_HTTP_AUTH=OFF
CONFLUX_BUILD_HTTP_COMPRESSION=OFF
CONFLUX_BUILD_HTTP_OBSERVABILITY=OFF
CONFLUX_BUILD_HTTP_OPENAPI=OFF
CONFLUX_BUILD_HTTP_PROXY=OFF

CONFLUX_BUILD_DB_POSTGRES=OFF
CONFLUX_BUILD_SMTP=OFF
```

### Backend flags (WITH)

```cmake
CONFLUX_WITH_TLS_OPENSSL=AUTO
CONFLUX_WITH_HTTP2_NGHTTP2=AUTO
CONFLUX_WITH_HTTP3_NGTCP2=AUTO

CONFLUX_WITH_GZIP_ZLIB=AUTO
CONFLUX_WITH_GZIP_LIBDEFLATE=AUTO
CONFLUX_WITH_GZIP_ZLIB_NG=AUTO
CONFLUX_WITH_GZIP_ISAL=AUTO
CONFLUX_WITH_BROTLI=AUTO
CONFLUX_WITH_ZSTD=AUTO

CONFLUX_WITH_POSTGRES_LIBPQ=AUTO
CONFLUX_WITH_JSON_STDSIMD=AUTO
CONFLUX_WITH_AESNI=AUTO
```

### CMake resolver sketch

```cmake
set(CONFLUX_FEATURE_SET "current" CACHE STRING
    "Feature bundle: current;core;runtime;json;http-minimal;http-api;http-api-full;web-server;http-server-complete;complete")

set_property(CACHE CONFLUX_FEATURE_SET PROPERTY STRINGS
    current core runtime json http-minimal http-api http-api-full web-server http-server-complete complete)

# Tri-state resolver — respects explicit user overrides, falls back to preset default
function(conflux_resolve out flag_name preset_default)
    set(v "${${flag_name}}")
    if(v STREQUAL "AUTO")
        set(${out} "${preset_default}" PARENT_SCOPE)
    elseif(v STREQUAL "ON")
        set(${out} TRUE PARENT_SCOPE)
    elseif(v STREQUAL "OFF")
        set(${out} FALSE PARENT_SCOPE)
    else()
        message(FATAL_ERROR "${flag_name}: expected AUTO|ON|OFF, got '${v}'")
    endif()
endfunction()
```

No `FORCE` on cache vars — explicit user overrides must survive preset changes.

### Preset expansion

```
core:
  BUILD_CORE=ON, everything else OFF

runtime:
  core + BUILD_RUNTIME + BUILD_SOCKET_IO

json:
  core + BUILD_JSON

http-minimal:
  runtime + BUILD_SOCKET_IO + BUILD_HTTP_CORE + BUILD_HTTP_ROUTER + BUILD_HTTP_SERVER

http-api:
  http-minimal + BUILD_JSON + BUILD_HTTP_JSON + BUILD_CRYPTO
  + BUILD_HTTP_POLICY + BUILD_HTTP_AUTH

http-api-full:
  http-api + BUILD_HTTP_OBSERVABILITY + BUILD_HTTP_OPENAPI

web-server:
  http-minimal + BUILD_FILE_IO + BUILD_HTTP_STATIC
  + BUILD_TEMPLATES + BUILD_HTTP_COMPRESSION + BUILD_HTTP_REALTIME
  + WITH_TLS_OPENSSL=AUTO + WITH_HTTP2_NGHTTP2=AUTO

http-server-complete:
  http-api ∪ web-server
  + BUILD_HTTP_CLIENT + BUILD_HTTP_PROXY + BUILD_DNS
  + WITH_HTTP3_NGTCP2=AUTO

complete:
  http-server-complete
  + BUILD_DB_POSTGRES + BUILD_SMTP + BUILD_PROCESS
  + BUILD_FILE_WATCH + BUILD_TEMPLATES_WATCH
  + BUILD_JSON_REFLECT (when compiler supports it)

current:
  today's defaults — all ON except JSON_REFLECT and WORK_CORO_FRAME_POOL
```

## User-facing ergonomics

After migration:

```cmake
# JSON-only tool
target_link_libraries(tool PRIVATE conflux::json)

# Minimal HTTP/1 server, no middleware
target_link_libraries(server PRIVATE conflux::http_server)

# REST API
target_link_libraries(api PRIVATE conflux::http_api)

# Static + TLS + templates web server
target_link_libraries(site PRIVATE conflux::web_server)

# Everything
target_link_libraries(app PRIVATE conflux::complete)
```

`conflux::conflux` stays as compatibility alias → preset-selected bundle.

## Implementation Priority

Do NOT alias existing monolith and call it done — that's false confidence with
no build/link win.

### Phase 1: Foundation (no API changes)

1. **Conditional dep discovery.** liburing, OpenSSL, compression libs become
   conditional on what's being built. `core`+`json` presets must configure
   without liburing.

2. **Extract always-real targets.** `conflux_json` already exists behind
   `CONFLUX_JSON_REFLECT` — make it always-built (it's the parser/DOM, not
   reflection). `conflux_crypto` as new target (currently inlined in conflux).

3. **`conflux::core` becomes real.** Types + utils + generated features.
   No liburing link.

### Phase 2: HTTP decoupling (API changes, additive)

4. **Move body_json out of http_request.** New `conflux.net.http.json` module,
   free functions. Old member functions get deprecation period or just remove
   (pre-v1, no stability promise yet).

5. **Split router.** Core routing into `router_core.cxx`. Static serving,
   SSE, WebSocket into separate modules. This is the hard one.

6. **Split template from file_watch.**

### Phase 3: Target wiring

7. **Create component targets.** Each maps to its module(s) + deps.

8. **Create meta/aggregate targets.** http_minimal, http_api, web_server, etc.

9. **Add preset resolver.** FEATURE_SET → BUILD defaults → dep discovery.

### Phase 4: Cleanup

10. **Rename DB target.** conflux_db → conflux_db_postgres + alias.

11. **Rename flags.** CONFLUX_ENABLE_* → CONFLUX_BUILD_*/WITH_* + aliases.

12. **Update examples.** Each links smallest needed target.

13. **Drop `current` preset** after one release cycle.

## Open Questions

1. **~~socket_io → file_io dependency~~**: **RESOLVED.** Incidental. `socket_io.cxx`
   imports only uring/completion/handle — no file_io. CMake `PUBLIC conflux_file_io`
   link is a stale artifact. Remove it. `http-minimal` does NOT need file_io.

2. **Module partition vs separate modules**: Should `conflux.net.http.json`
   be a partition of `conflux.net.http` or a standalone module? Partitions
   can access internals but must be in same target. Standalone modules have
   cleaner dep graphs but can't access private state.

3. **~~Pre-v1 stability promise~~**: **RESOLVED.** Pre-release, break freely.
   Remove `body_json` member fns, replace with free fns in `http_json`.
   Document overload works (calls `doc.dump()`). NodeRef overload is broken
   anyway — assigns to throwaway `_`, never serializes. No deprecation cycle.

4. **Test/bench target selection**: Should `CONFLUX_BUILD_TESTS` build tests
   for all built components, or should each component have its own test flag?
   Current approach (all-or-nothing) is simpler but slow for component-focused
   dev.

5. **Header fallback for core**: `conflux::core` with no liburing could
   potentially be header-only or very thin. Worth considering interface-only
   target for types/utils?

6. **Compression backend selection**: Current code selects best available at
   runtime. With per-backend WITH flags, does AUTO still mean "use whatever's
   found" or should presets pick a preferred backend? Leaning toward: AUTO =
   probe all, use best. User can force one with explicit ON + others OFF.

7. **Cross-module lambda issue (GCC)**: Recent commit `07c7da3` fixed a
   cross-module lambda problem. Will splitting router into more modules
   create more instances of this? Need to audit lambda usage in router
   before splitting.

## Verdict

**Implement, with revisions below.** Direction correct. Main risk is not CMake
wiring — it is that public types encode optional features directly
(`HttpResponse::BodyPayload` variant names SSE/WS/MappedFile/StreamedFile).

### Code verification (2026-05-11)

| Claim | Status | Detail |
|-------|--------|--------|
| `conflux.cxx` re-exports 5 modules | **exact** | types, file_watch, templates, json, net.http — nothing else |
| `conflux.net.http` re-exports 35+ | **confirmed** | 30 net.* modules, 3 conditional (jwt/metrics/http2) |
| liburing REQUIRED unconditionally | **confirmed** | `Dependencies.cmake:28` |
| `http_request.cxx` imports json for body_json | **confirmed** | Document overload serializes via `dump()`. **NodeRef overload broken** — assigns to `_`, never serializes |
| `template.cxx` imports file_watch | **confirmed** | FileWatcher for hot-reload, UP\<FileWatcher\> member |
| Router imports crypto/work/file_io/socket_io/config/tls | **confirmed** | all present |
| Router ~2500 lines | **wrong** | actually **3069 lines** |
| socket_io → file_io is incidental | **confirmed** | socket_io.cxx imports uring/completion/handle only; CMake PUBLIC link is stale |
| HttpResponse embeds SSE/WS/MappedFile/StreamedFile | **confirmed** | `BodyPayload` variant at router.cxx:550 |
| http_server imports vhost unconditionally | **confirmed** | `import conflux.net.vhost;` |
| vhost missing from target list | **confirmed** | mentioned in problem statement, absent from target map |
| sync client imports file_io "for wait_fd" | **wrong** | `wait_fd` lives in `utils.cxx`. client.cxx imports file_io but **uses nothing from it** — stale import |
| async client uses socket_io | **confirmed** | imports socket_io + socket_io.coro + uring.completion + cancel |
| JWT gated behind CONFLUX_HAS_TLS | **confirmed** | CMakeLists.txt:420-424 |
| jwt.cxx imports crypto, not TLS | **confirmed** | imports crypto, types, http.types, router, utils |
| conflux_json only exists behind CONFLUX_JSON_REFLECT | **confirmed** | otherwise json.cxx compiled into monolith |
| crypto.cxx has no TLS dep | **confirmed** | imports std, types, std.compat only |
| No conflux_crypto CMake target | **confirmed** | crypto.cxx in monolith CONFLUX_MODULE_SOURCES |

### Required changes before implementation

**1. `runtime` must not imply sockets.**

socket_io → file_io CMake link is stale (socket_io.cxx never imports file_io).
Remove `PUBLIC conflux_file_io` from `conflux_socket_io` target first. Then:

```
conflux::runtime   = types + uring + work
conflux::socket_io = runtime + socket modules
conflux::file_io   = runtime + file modules
```

**2. Add `conflux::http_vhost` target.**

http_server.cxx imports `conflux.net.vhost` unconditionally. Vhost changes
dispatch topology — not policy middleware. Either:
- Separate target: `conflux::http_vhost` depends http_router + work
- Or fold into http_server (accept the coupling)

**3. Split sync/async HTTP client.**

client.cxx is blocking POSIX, imports file_io but uses nothing from it (stale).
client_async.cxx uses socket_io/coro/uring.completion/cancel.

```
conflux::http_client_sync   = http types + request + utils + optional TLS
conflux::http_client_async  = http_client_sync + socket_io + dns + cancel
conflux::http_client        = aggregate (async by default)
```

Preserves no-liburing blocking client for tests/tools.

**4. Keep OpenAPI out of default `http_api`.**

```
conflux::http_api            = http_minimal + http_json + http_policy + http_auth
conflux::http_observability  = metrics + structured_log + tracing + request_id
conflux::http_openapi        = OpenAPI
conflux::http_api_full       = http_api + observability + openapi
```

**5. Response body redesign before router split.**

`HttpResponse::BodyPayload` at router.cxx:550:
```cpp
variant<S,SP<SseChannel>,SP<WsUpgrade>,SP<MappedFile>,SP<StreamedFile>,SP<DeferredResponse>>
```

Core response type names optional feature types. Router cannot become core-only
until this is resolved. Required pre-refactor:

```
Phase 2a: split response body model
  - core: text/body bytes, status, headers, cookies, trailers
  - core: neutral deferred/stream body abstraction
  - extension: file region / mapped file / streamed file factories
  - extension: SSE/WS response factories

Phase 2b: split router (after body model is clean)
  - router_core: route matching, params, groups, middleware
  - http_static: serve_static + file cache + range + etag
  - http_realtime: SSE/WS registration
```

**6. Make `conflux_json` always real.**

Currently json.cxx is in monolith unless CONFLUX_JSON_REFLECT=ON extracts it.
Reverse this: always extract conflux_json. Reflect depends on json, not the
other way around.

**7. Extract `conflux_crypto` now.**

crypto.cxx imports only std/types/std.compat — already standalone in code,
just not in CMake. Extract before HTTP target work → less churn for auth/jwt/ws.

**8. Fix stale imports early.**

- Remove `import conflux.file_io` from client.cxx (unused)
- Remove `PUBLIC conflux_file_io` from conflux_socket_io CMake target
- Fix or remove broken `body_json(NodeRef)` overload

### Revised implementation order

1. **Fix stale deps + broken APIs** — client.cxx file_io import, socket_io CMake
   link, body_json(NodeRef) removal
2. **Feature resolver** — CONFLUX_FEATURE_SET, CONFLUX_BUILD_*/WITH_* resolution,
   conditional liburing discovery
3. **Foundation targets** — conflux_core, conflux_json (always), conflux_crypto,
   conflux_process
4. **Remove incidental coupling** — template/file_watch split, move body_json
   to http_json free fns
5. **Response body redesign** — decouple BodyPayload from SSE/WS/static types
6. **Router split** — router_core, http_static, http_realtime
7. **HTTP target wiring** — server, client_sync/async, policy, auth, vhost,
   observability, openapi, proxy
8. **Aggregates + presets** — http_minimal, http_api, web_server, etc.
9. **Tests/benches/examples** — link smallest targets, component test toggles

### Effort estimate

CMake wiring is not the main cost. Response/router/server separation is.

```
Fix stale deps + broken APIs:           1–2 days
Feature resolver + conditional deps:    3–5 days
Foundation targets (json/crypto/core):  3–5 days
Response body + router split:           1.5–3 weeks
HTTP target wiring + examples/tests:    1–2 weeks
Cleanup/polish:                         3–5 days
Total:                                  4–7 weeks
```
