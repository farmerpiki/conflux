# conflux

`conflux` is a modules-first Linux-only C++26 runtime and networking library built around
`io_uring`. The preview surface focuses on the runtime core, JSON, HTTP server
building blocks, and PostgreSQL support when the DB evidence lane is green.

It is not a portability layer for non-Linux systems, and it is not a stable-v1
API yet. Public names may still change before v1 when the change removes
compatibility clutter or fixes an incorrect contract.

## Requirements

- Linux. Runtime-facing components also need `io_uring` enabled and available
  to the current user/container.
- `pkg-config`; `liburing` development headers and library are required only
  when building runtime-facing components.
- CMake 3.30 or newer and Ninja for the current known-good prerelease lanes.
- A C++23/26-capable compiler matching one of the provided CMake presets.

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

Use the checked-in presets as the support matrix. `MODULE_INTERFACE` is the
primary source-consumption and development mode. `CONFLUX_USE_IMPORT_STD` is a
separate `AUTO|ON|OFF` knob: `AUTO` uses the standard-library module when
CMake/toolchain support is discoverable, while `OFF` keeps `MODULE_INTERFACE`
and lets CMake generate a source overlay that replaces `import std` with
ordinary standard headers. Generated headers are staged release artifacts for
compatibility consumers and should not require CMake import-std discovery.

## First-contact docs

Read the release-facing docs in this order:

1. [`README.md`](README.md)
2. [`docs/package-consumption.md`](docs/package-consumption.md)
3. [`docs/component-map.md`](docs/component-map.md)
4. [`docs/public-api-map.md`](docs/public-api-map.md)
5. [`docs/http-server-api.md`](docs/http-server-api.md) or [`docs/json-api.md`](docs/json-api.md)
6. [`docs/cost-lifetime-model.md`](docs/cost-lifetime-model.md)
7. [`docs/production-checklist.md`](docs/production-checklist.md)
8. [`docs/release-checklist.md`](docs/release-checklist.md)

Maintainer planning files are outside this first-contact path.

## Install and consume

After installing the package, consume only the components your program needs:

```cmake
find_package(conflux REQUIRED COMPONENTS core json http)
target_link_libraries(myapp PRIVATE conflux::core conflux::json conflux::http)
```

The package contract, component/target map, public import map, and
copy/allocation/lifetime model are covered by the first-contact docs above.

## Small HTTP app

```cpp
import conflux.http;
import std;

namespace http = conflux::http;

int main() {
    auto app = http::app();

    app.get("/", [] { return http::text("hello from conflux\n"); });

    return static_cast<int>(std::move(app).run({.port = 8080}));
}
```

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
./build/debug-gcc-stdcxx/tests/conflux_work_tests
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
- C++26 modules support is toolchain-sensitive.
- `CONFLUX_USE_MOCK_LIBURING=ON` proves buildability only, not runtime support.
- Runtime proof is maintained as separate release evidence and finalized only
  after the release-candidate source tree is frozen.
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
