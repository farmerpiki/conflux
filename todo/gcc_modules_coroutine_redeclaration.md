# GCC Modules Coroutine Redeclaration Blocker

Status: resolved by avoiding the exported template coroutine frame in
`App::run_app_task_response`.

Latest verification:

- `cmake --build --preset debug-gcc-stdcxx --target conflux_tests conflux_http_facade_extractors_tests`
- `scripts/run-ctest.sh --test-dir build/debug-gcc-stdcxx --output-on-failure -R "^(http app:|http facade:)"`
- `cmake --build --preset debug-gcc16-stdcxx --target conflux_tests conflux_http_facade_extractors_tests`
- `scripts/run-ctest.sh --test-dir build/debug-gcc16-stdcxx --output-on-failure -R "^(http app:|http facade:)"`
- `cmake --build --preset debug-clang-libcxx --target conflux_tests conflux_http_facade_extractors_tests`
- `scripts/run-ctest.sh --test-dir build/debug-clang-libcxx --output-on-failure -R "^(http app:|http facade:)"`

Both affected test binaries now build on the GCC 15, GCC 16, and Clang/libc++
debug lanes, and the focused HTTP app/facade CTest selections pass.

## Error Signature

GCC 15.2.1 in the `debug-gcc-stdcxx` lane fails while compiling existing async
HTTP app test translation units:

```text
error: redeclaring 'struct ... run_app_task_response ... .Frame' in global module conflicts with import
note: import declared attached to module 'conflux.net.app'
```

The failure happens before upload-specific test assertions can run.

## Tried And Failed

- `cmake --build --preset debug-gcc-stdcxx --target conflux_tests conflux_http_facade_extractors_tests`
  fails while compiling `conflux_http_facade_extractors_tests`, triggered by the
  existing `/async-todos/{id:i64}` route in `tests/http_facade_extractors_test.cxx`.
- `cmake --build --preset debug-gcc-stdcxx --target conflux_tests` fails while
  compiling `tests/http_app_e2e.cxx.o`, triggered by the existing
  `/async-fixed-secret` route in `tests/http_app_e2e.cxx`.
- Retrying the narrower `conflux_tests` target does not bypass the problem
  because the target still compiles the full affected test translation unit.

## Resolution

`src/net/app.cxx` now keeps `run_app_task_response` as a non-coroutine template
and returns `work::root::make_cancellable_task(...)` around a non-exported inner
coroutine lambda. That preserves the async handler behavior while avoiding a
coroutine frame attached inconsistently across the `conflux.net.app` module
boundary.

## Do Not Retry Blindly

- Do not keep rerunning the full GCC test targets expecting upload-source changes
  to affect this failure.
- Do not chase `UploadBody` or HTTP transport code for this diagnostic.
- Do not hide the problem by removing upload tests; the blocker is an existing
  GCC modules/coroutine-frame interaction involving async app route machinery
  imported from `conflux.net.app`.
