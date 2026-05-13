# Contract / Design-Rule Fixes

## Hard breaks

- [ ] CMake: finish public component split per modular proposal. Foundation: `conflux::core` (no liburing), `conflux::file_io_sync` (POSIX sync, no liburing), `conflux::file_map` (standalone module, no liburing), `conflux::runtime`, `conflux::file_io`, `conflux::socket_io`, `conflux::dns`, `conflux::crypto` (extracted early), `conflux::json` (always real target, not gated behind REFLECT), `conflux::json_file`, `conflux::template`, `conflux::template_watch`. HTTP: `conflux::http_core`, `conflux::http_router`, `conflux::http_server`, `conflux::http_static_core` (sync metadata/mmap/etag/range, depends file_io_sync + file_map), `conflux::http_static_async` (streamed/splice, depends file_io), `conflux::http_auth` (JWT decoupled from TLS gate), `conflux::http_json`, `conflux::http_policy`, `conflux::http_observability`, `conflux::http_openapi` (separate from http_api; http_api_full aggregate includes OpenAPI), `conflux::http_realtime`, `conflux::http_vhost`, `conflux::http_client_sync`, `conflux::http_client_async`; add install/export/namespaced package targets
  - [x] First packaging slice: existing modular targets now have build-tree aliases and install/export names (`conflux::core`, `conflux::types`, `conflux::runtime`, `conflux::file_io_sync`, `conflux::file_map`, `conflux::file_io`, `conflux::socket_io`, `conflux::dns`, `conflux::crypto`, `conflux::json`, `conflux::db`, `conflux::http_server`); package config restores required PkgConfig/OpenSSL/ZLIB deps.
  - [x] Template split slice: `src/template.cxx` and `src/file_watch.cxx` are separate install/export module targets (`conflux::template`, `conflux::template_watch`) instead of being compiled into the HTTP monolith; umbrella `conflux` re-exports them only when present. Remaining work: split not-yet-separated HTTP/json_file/static/client component targets instead of aliasing the monolith.
- [ ] Handler model: normalize all public handlers to one internal shape `Task<Response>(Request)`; sync lambdas auto-offload or require `nonblocking` annotation — never execute arbitrary user sync code on ring thread (`src/net/router.cxx:2792-2824`, `http_server.cxx:4055-4063`)
- [ ] Public API: remove exported type aliases (`S`, `SV`, `SP`, `Opt`, etc. in `src/types.cxx:15-75`) from public signatures; use spelled-out std types at all public boundaries
- [x] JSON default path: `parse(string_view)` now aliases the borrowed/view path; explicit owning parse moved to `parse_copy(string_view|string&&)`; rvalue `parse(string&&)` is deleted; docs/examples promote `parse_view` as the primary fast API
- [x] JSON: add `JsonArena::parse_borrowed_into(string_view)` and `parse_moved_into(string&&)` — both landed; `parse_borrowed_into` reuses caller's buffer without copy, `parse_moved_into` takes ownership.
- [x] Core error type / `file_io_sync`: `IoError` is exported by `conflux.types`; `file_io_sync` no longer imports or links `conflux.uring.completion` / `conflux_uring`. Full `conflux::core` target split remains tracked separately.
- [x] xxhash: resolve via `pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)` and link `PkgConfig::XXHASH` instead of a dangling raw `xxhash` target/name
- [x] JSON global state: document `CLocaleHolder` singleton as the only permitted process-lifetime singleton; design note added in `docs/json-design.md`

## Incomplete perf quick-wins

- [x] Direct-accept `TCP_NODELAY`: landed
- [x] `MADV_DONTFORK` for `FixedBufferPool` slabs: landed
- [x] Setup-flag fallback: landed (try-init/strip matrix, log requested vs active)
- [x] `NO_SQARRAY`: landed (default on, stripped on unsupported kernel)
- [x] `alignas(64)` on `Conn` and `Worker`: landed (`struct alignas(64) Conn`, `struct alignas(64) Worker`)
- [ ] `Ring` hot/cold field layout: `Ring` has no `alignas`; verify `Conn`/`Worker` field grouping with `perf c2c` before further padding
- [x] Root `Task<T>` allocation diagnostics: optional `CONFLUX_WORK_ALLOC_STATS` counters for control blocks and coroutine frames landed
- [ ] Coroutine frame/control-block pool: landed pool targets `EagerChainPromise`, not `root::Task<T>::promise_type` which is the HTTP handler hot path; pool `BasicResult<T,task>::promise_type` frames and `ControlBlockModel<T>` after measuring allocation counters

## Docs / API contract mismatches

- [x] `docs/json-api.md:49-51`: update limits to match code (`max_input = 128 MiB`, `max_string = 64 MiB`); docs still say 4 GiB / unlimited (`src/json.cxx:661-663`)
