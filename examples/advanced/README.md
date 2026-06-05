# Advanced examples

These examples intentionally show extended, low-level, or specialized Conflux
APIs: runtime work pools, cost-model boundaries, explicit offload, protocol experiments, manual JSON
mapping, observability policy stacks, production lifecycle composition,
database primitives, process execution, templates, and crypto utilities.
`postgres_json.cxx` shows the HTTP app facade combined with PostgreSQL
coroutines, so it lives here rather than in the first-contact quickstart set.

Examples that compose stable production customization surfaces should prefer
`import conflux.extended;`. Examples that teach a specific low-level subsystem
should keep explicit leaf imports such as `conflux.file_io`, `conflux.work`, or
`conflux.net.http.server`.

Start with `examples/quickstart/` or the top-level HTTP examples when learning
the selected curated umbrella surface. Use this directory when you need to
customize internals or inspect lower-level building blocks.
