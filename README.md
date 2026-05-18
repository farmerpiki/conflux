# conflux

`conflux` is a Linux-only C++26 runtime and networking library built around
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
- CMake 4.2 or newer and Ninja.
- A C++26-capable compiler matching one of the provided CMake presets.

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

Use the checked-in presets as the support matrix. C++26 modules remain sensitive
to the exact compiler, standard library, and CMake versions.

## Install and consume

After installing the package, consume only the components your program needs:

```cmake
find_package(conflux REQUIRED COMPONENTS core json http)
target_link_libraries(myapp PRIVATE conflux::core conflux::json conflux::http)
```

The component/target map lives in [`docs/component-map.md`](docs/component-map.md).

## Small HTTP app

```cpp
import conflux.net.http.server;
import std;

namespace http = conflux::http;

int main() {
    auto app = http::App::default_server();

    app.get("/", [](http::Request const &) {
        return http::Response::text("hello from conflux\n");
    });

    auto const status = move(app).run({.port = 8080});
    return status == http::RunStatus::stopped_normally ? 0 : 1;
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
- Preview APIs may still break before v1.

## License

Conflux is licensed under the Apache License, Version 2.0. See
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).

## Component and package usage

Use `CONFLUX_FEATURE_SET` to select a bundle and `find_package(conflux REQUIRED
COMPONENTS ...)` to consume installed targets. The current component/target map
lives in [`docs/component-map.md`](docs/component-map.md); CI/package validation
commands live in [`docs/build-ci-lanes.md`](docs/build-ci-lanes.md).
