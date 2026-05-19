# Quickstart examples

These examples show the first-contact HTTP app facade. They intentionally use
the smallest module surface for each workflow and avoid lower-level runtime,
provider, and raw response APIs.

Most examples import only `conflux.http`. `postgres_json.cxx` also imports
`conflux.pg` because the database pool is still a separate curated module.

Use `examples/advanced/` for explicit providers, work pools, raw DB/runtime
setup, HTTP/3, and other lower-level integration paths.
