# GCC Modules Coroutine Redeclaration Blocker

Status: verified blocker, not caused by the streaming upload changes.

Latest verification: `cmake --build --preset debug-gcc-stdcxx --target conflux_tests`
still fails in `tests/http_app_e2e.cxx.o` on the existing
`/async-fixed-secret` route.

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

## Do Not Retry Blindly

- Do not keep rerunning the full GCC test targets expecting upload-source changes
  to affect this failure.
- Do not chase `UploadBody` or HTTP transport code for this diagnostic.
- Do not hide the problem by removing upload tests; the blocker is an existing
  GCC modules/coroutine-frame interaction involving async app route machinery
  imported from `conflux.net.app`.

## Next Investigation Direction

If fixing the GCC lane, start at the async app route template coroutine path in
`src/net/app.cxx`, especially `run_app_task_response` and fixed-route async
dispatch. The likely direction is to avoid instantiating a template coroutine
with its frame attached inconsistently across the imported module boundary.
Before changing that area, also check the GCC runbook mentioned by
`coding_standards/core_workflow.md`.
