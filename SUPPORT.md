# Support Policy

Conflux is pre-v1 preview software. Support is scoped to the latest prerelease
tag and the release SKU documented for that tag.

## Preview SKUs

- `release-json`: JSON parser, DOM/view APIs, codec APIs, generated headers,
  modules, and package consumers for the JSON-focused component set.
- `release-http-api`: HTTP app, router, client/server API surface, package
  consumption, and examples selected for API preview.
- `release-web-server`: HTTP framework behavior, lifecycle, security defaults,
  observability, and deployment-facing configuration covered by release
  evidence.

`release-full` is not a first-contact preview SKU unless a release explicitly
chooses it.

## Toolchains

Supported compiler, CMake, Ninja, kernel, and dependency baselines are the
lowest versions that pass the release evidence matrix. Newer local toolchains
may be used for development, but they do not define public support
requirements.

C++ modules, `import std`, reflection, and generated-header artifacts are
supported only in the interface modes and compiler lanes documented by the
release evidence.

Operational release gates live in
[`docs/release-checklist.md`](docs/release-checklist.md). The broader project
support policy lives in [`docs/project-policy.md`](docs/project-policy.md).
