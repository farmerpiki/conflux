# Quickstart examples

These examples show the first-contact HTTP app facade through the selected
umbrella import. Under the default `CONFLUX_API_SURFACE=curated`, `import
conflux;` exposes the curated HTTP/JSON app surface without teaching lower-level
runtime, provider, or raw response APIs.

Most examples import only `conflux`. `json_reflect_crud.cxx` adds an explicit
leaf import for reflection because that feature-specific API remains opt-in.

Handlers run on the HTTP runtime/ring context. Keep quickstart handlers short
and non-blocking; use the advanced explicit offload/work-pool examples for disk,
DNS, database, client HTTP, sleeps, contended locks, or CPU-heavy work. Request
bodies are bounded and buffered in memory in the preview server API.

`json_crud.cxx` and `json_reflect_crud.cxx` use in-memory demo state only for
local curl experiments. Do not copy that storage shape into services;
production handlers should call non-blocking storage or explicitly leave the
ring context before blocking or contending.

Use `examples/advanced/` for `conflux.extended`, explicit providers, work pools,
database coroutines, raw runtime setup, HTTP/3, and other lower-level
integration paths.
