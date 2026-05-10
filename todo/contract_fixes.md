# Contract / Design-Rule Fixes

## Hard breaks

- [ ] CMake: split `conflux` monolith into `conflux_core`, `conflux_http`, `conflux_json`, `conflux_auth`, `conflux_metrics`, `conflux_tracing`, `conflux_openapi`, `conflux_http_tls_openssl`; keep `conflux` as opt-in umbrella only (`CMakeLists.txt:374-445`)
- [ ] Handler model: normalize all public handlers to one internal shape `Task<Response>(Request)`; sync lambdas auto-offload or require `nonblocking` annotation — never execute arbitrary user sync code on ring thread (`src/net/router.cxx:2792-2824`, `http_server.cxx:4055-4063`)
- [ ] Public API: remove exported type aliases (`S`, `SV`, `SP`, `Opt`, etc. in `src/types.cxx:15-75`) from public signatures; use spelled-out std types at all public boundaries
- [ ] JSON default path: rename `parse(string_view)` → `parse_copy`; promote `parse_borrowed` as the performance-default; document `parse_view`/`json::view_document` as the primary fast API (`src/json.cxx:4103-4111`)
- [x] JSON: add `JsonArena::parse_borrowed_into(string_view)` and `parse_moved_into(string&&)` — both landed; `parse_borrowed_into` reuses caller's buffer without copy, `parse_moved_into` takes ownership.
- [ ] xxhash: either vendor properly with `find_package`/FetchContent or replace with internal hash; resolve the dangling `CMakeLists.txt:492` link with no declared source
- [ ] JSON global state: document `CLocaleHolder` singleton (`src/json.cxx:669-675`) as the only permitted process-lifetime singleton; add design note

## Incomplete perf quick-wins (marked done, actually not)

- [x] Direct-accept `TCP_NODELAY`: set `setup.tcp_nodelay_once = caps.cmd_sock_setsockopt` in `queue_direct_accept_setup()` (`http_server.cxx:1794-1796`)
- [x] `MADV_DONTFORK` for `FixedBufferPool` slabs: add `madvise(MADV_DONTFORK)` + `MADV_HUGEPAGE` in `FixedBufferPool` ctor (`file_io.cxx:101-111`)
- [ ] Setup-flag fallback: implement try-init/strip matrix for `DEFER_TASKRUN`, `SINGLE_ISSUER`, `NO_SQARRAY`, etc.; log requested vs active flags; current code hard-throws on `EINVAL` (`http_server.cxx:3728-3747`)
- [ ] `NO_SQARRAY`: request by default, clear on unsupported kernel; currently config-off-by-default (`config.cxx:128`, `http_server.cxx:3743-3744`)
- [ ] Coroutine frame pool: landed pool targets `EagerChainPromise`, not `root::Task<T>::promise_type` which is the HTTP handler hot path; add `operator new/delete` on `BasicResult<T,task>::promise_type` and pool `ControlBlockModel<T>` (`root.cxx:1920-1930`, `1740-1747`)

## Docs / API contract mismatches

- [x] `docs/json-api.md:49-51`: update limits to match code (`max_input = 128 MiB`, `max_string = 64 MiB`); docs still say 4 GiB / unlimited (`src/json.cxx:661-663`)
