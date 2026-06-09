# conflux

`conflux` is a modules-first Linux-only C++23-baseline runtime and networking
library built around `io_uring`, with C++26-gated reflection and standard-SIMD
experiments. The preview surface focuses on the runtime core, JSON, HTTP server
building blocks, and PostgreSQL support when the DB evidence lane is green.

It is not a portability layer for non-Linux systems, and it is not a stable-v1
API yet. Public names may still change before v1 when the change removes
compatibility clutter or fixes an incorrect contract.

## Requirements

- Linux. Runtime-facing components also need `io_uring` enabled and available
  to the current user/container.
- `pkg-config`; `liburing` development headers and library are required only
  when building runtime-facing components.
- CMake 4.2 or newer and Ninja for the advertised prerelease support
  lanes. Older CMake versions may continue to configure opportunistically when
  the project files allow it, but they are not part of the release support
  contract until a green evidence lane is recorded for them.
- One of the tested compiler families from the support matrix: GCC 15, GCC 16,
  or Clang 21. Optional reflection currently requires the GCC 16 P2996 lane;
  optional standard-SIMD feature targets require a C++26-capable toolchain with
  the matching library feature probe.

Optional protocol, storage, and runtime-facing features are enabled when their
libraries are available or selected by the feature bundle, including `liburing`,
OpenSSL, nghttp2, ngtcp2/nghttp3, libpq, and compression backends. The
`core`, `json`, `file_io_sync`, and `file_map` component surfaces remain
liburing-free.

## Build from source

```sh
cmake --preset release-clang-libcxx
cmake --build --preset release-clang-libcxx
```

Use the checked-in presets as the support matrix, and treat the exact compiler
versions attached to the release evidence as authoritative. `MODULE_INTERFACE`
is the primary source-consumption and development mode. `CONFLUX_USE_IMPORT_STD`
is a separate `AUTO|ON|OFF` knob: `AUTO` uses the standard-library module when
CMake/toolchain support is discoverable, while `OFF` keeps `MODULE_INTERFACE`
and lets CMake generate a source overlay that replaces `import std` with
ordinary standard headers. Generated headers are staged release artifacts for
compatibility consumers that cannot use modules; they are not the design center
for new API work and are supported only for the components and toolchains
covered by the matching release evidence.

## First-contact docs

Read the release-facing docs in this order:

1. [`README.md`](README.md)
2. [`docs/package-consumption.md`](docs/package-consumption.md)
3. [`docs/component-map.md`](docs/component-map.md)
4. [`docs/public-api-map.md`](docs/public-api-map.md)
5. [`docs/task-path.md`](docs/task-path.md)
6. [`docs/json-api.md`](docs/json-api.md), [`docs/json-boundary-guide.md`](docs/json-boundary-guide.md), or [`docs/json-cookbook.md`](docs/json-cookbook.md)
7. [`docs/http-server-api.md`](docs/http-server-api.md) or [`docs/conflux-http-client-api.md`](docs/conflux-http-client-api.md)
8. [`docs/db-api.md`](docs/db-api.md) for PostgreSQL pipeline support (`pg` component)
9. [`docs/api-surface-profiles.md`](docs/api-surface-profiles.md)
10. [`docs/extension-points.md`](docs/extension-points.md)
11. [`docs/observability.md`](docs/observability.md) and [`docs/configuration.md`](docs/configuration.md)
12. [`docs/cost-lifetime-model.md`](docs/cost-lifetime-model.md)
13. [`docs/production-checklist.md`](docs/production-checklist.md)
14. [`docs/prerelease-status.md`](docs/prerelease-status.md)
15. [`docs/release-checklist.md`](docs/release-checklist.md)

Maintainer planning files are outside this first-contact path.

## Install and consume

After installing the package, consume only the components your program needs:

```cmake
find_package(conflux REQUIRED COMPONENTS http json)
target_link_libraries(myapp PRIVATE conflux::http conflux::json)
```

The package contract, component/target map, public import map, and
copy/allocation/lifetime model are covered by the first-contact docs above.

## Small HTTP app

```cpp
import conflux;
import std;

namespace http = conflux::http;

int main() {
    auto app = http::app();

    app.get("/", [] { return http::text("hello from conflux\n"); });

    return http::exit_code(std::move(app).run({.port = 8080}));
}
```

More first-contact examples live under `examples/quickstart/`. They use the
curated `import conflux;` facade, typed path/body helpers, and short
ring-thread-safe handlers. Lower-level runtime, raw router, explicit offload,
DB, and protocol experiments live under `examples/advanced/`.

## JSON only

```cpp
import conflux.json;

using namespace conflux::json;

int main() {
    auto doc = parse(R"({"ok":true,"n":42})");
    if (!doc) {
        return 1;
    }
    return doc->root()["ok"].as_bool().value_or(false) ? 0 : 1;
}
```

## Project policy

Versioning, security disclosure, supported compiler presets, and kernel/runtime support are documented in [`docs/project-policy.md`](docs/project-policy.md).
Root release governance entrypoints are [`SECURITY.md`](SECURITY.md),
[`SUPPORT.md`](SUPPORT.md), [`CHANGELOG.md`](CHANGELOG.md), and
[`RELEASE_POLICY.md`](RELEASE_POLICY.md).
The task/HTTP placement contract is documented in [`docs/execution-model.md`](docs/execution-model.md),
and the code-review guide for concurrency/naming decisions is
[`docs/concurrency-naming-model.md`](docs/concurrency-naming-model.md). The pre-v1
execution-name cleanup inventory is documented in [`docs/naming-audit.md`](docs/naming-audit.md).
Component bundles, package targets, primary module imports, and API/doc ownership
are indexed in [`docs/component-map.md`](docs/component-map.md). The pre-v1
release evidence checklist lives in [`docs/release-checklist.md`](docs/release-checklist.md).
Cost, allocation, blocking, and borrow lifetimes are summarized in
[`docs/cost-lifetime-model.md`](docs/cost-lifetime-model.md).

## Runtime Preflight

Before running the server or runtime-facing test binaries on a new host, confirm
that the environment permits `io_uring`:

```sh
cmake --preset debug-gcc-stdcxx
cmake --build --preset debug-gcc-stdcxx --target conflux_work_tests
BUILD_DIR=/tmp/conflux/debug-gcc-stdcxx
./scripts/run-build-artifact.sh "$BUILD_DIR/tests/conflux_work_tests"
```

If `io_uring_queue_init` or `io_uring_queue_init_params` fails, the host does
not satisfy conflux runtime requirements. Common causes are an old kernel,
container seccomp restrictions, disabled `io_uring`, or limits that prevent ring
setup.

## Known limitations

- Linux is a hard requirement.
- `io_uring` and `liburing` are required for runtime-facing components.
- Containers or seccomp policy can block runtime setup.
- Optional protocol/storage dependencies are feature-gated.
- Module and import-std support is toolchain-sensitive and supported only for
  the tested GCC 15, GCC 16, and Clang 21 lanes named in release evidence;
  optional reflection and standard-SIMD targets are C++26-gated.
- Runtime and benchmark evidence are release artifacts, not tracked source-tree
  payloads.
- DB examples require DB support enabled and usable libpq headers.
- Preview APIs may still break before v1.

## License

Conflux is licensed under the Apache License, Version 2.0. See
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).

## Component and package usage

Use `CONFLUX_FEATURE_SET` to select a bundle and `find_package(conflux REQUIRED
COMPONENTS ...)` to consume installed targets. The current component/target map
lives in [`docs/component-map.md`](docs/component-map.md); CI/package validation
commands live in [`docs/build-ci-lanes.md`](docs/build-ci-lanes.md).

## Quick try with auto-detected features

For local source evaluation, Conflux can enable first-contact stable components
whose dependencies are already installed on your machine:

```sh
cmake -S . -B /tmp/conflux/auto -DCONFLUX_FEATURE_SET=auto
cmake --build /tmp/conflux/auto
```

`CONFLUX_FEATURE_SET=auto` is meant for quickly trying Conflux without learning
every optional feature flag first. Do not use `auto` for packaging,
reproducible CI, or published benchmarks. Use an explicit feature set for those
cases.
