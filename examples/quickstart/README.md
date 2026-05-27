# Quickstart examples

These examples show the first-contact HTTP app facade through the selected
umbrella import. Under the default `CONFLUX_API_SURFACE=curated`, `import
conflux;` exposes the curated HTTP/JSON app surface without teaching lower-level
runtime, provider, or raw response APIs.

Most examples import only `conflux`. `json_reflect_crud.cxx` and
`postgres_json.cxx` add explicit leaf imports for reflection and PostgreSQL
because those feature-specific APIs remain opt-in.

Handlers run on the HTTP runtime/ring context. Keep quickstart handlers short
and non-blocking; use the advanced explicit offload/work-pool examples for disk,
DNS, database, client HTTP, sleeps, or CPU-heavy work. Request bodies are
bounded and buffered in memory in the preview server API.

Use `examples/advanced/` for `conflux.extended`, explicit providers, work pools,
raw DB/runtime setup, HTTP/3, and other lower-level integration paths.
