# conflux

`conflux` is a Linux-only C++26 networking and runtime project built around
`io_uring`. A working `io_uring` runtime is a hard requirement: the HTTP server,
file I/O layer, work-ring integration, tests, and benchmarks assume that the
host kernel allows `io_uring_queue_init*`.

## Requirements

- Linux with `io_uring` enabled and available to the current user/container.
- `liburing` development headers and library discoverable through `pkg-config`.
- CMake 4.2 or newer and Ninja.
- A C++26-capable compiler matching one of the provided CMake presets.

Optional protocol and storage features are enabled when their libraries are
available, including OpenSSL, nghttp2, ngtcp2/nghttp3, libpq, and compression
backends. These are feature-gated; `io_uring` and `liburing` are not.

## Project Policy

Versioning, security disclosure, supported compiler presets, and kernel/runtime support are documented in [`docs/project-policy.md`](docs/project-policy.md).
The task/HTTP placement contract is documented in [`docs/execution-model.md`](docs/execution-model.md),
and the code-review guide for concurrency/naming decisions is
[`docs/concurrency-naming-model.md`](docs/concurrency-naming-model.md). The pre-v1
execution-name cleanup inventory is documented in [`docs/naming-audit.md`](docs/naming-audit.md).
Component bundles, package targets, primary module imports, and API/doc ownership
are indexed in [`docs/component-map.md`](docs/component-map.md). The pre-v1
release evidence checklist lives in [`docs/release-checklist.md`](docs/release-checklist.md).

## Runtime Preflight

Before running the server or the core test binaries on a new host, confirm that
the environment permits `io_uring`:

```sh
./build/debug-gcc-stdcxx/tests/conflux_work_tests
```

If `io_uring_queue_init` or `io_uring_queue_init_params` fails, the host does
not satisfy conflux runtime requirements. Common causes are an old kernel,
container seccomp restrictions, disabled `io_uring`, or limits that prevent ring
setup.

## Build

```sh
cmake --preset debug-gcc-stdcxx
cmake --build --preset debug-gcc-stdcxx
```

## Component and package usage

Use `CONFLUX_FEATURE_SET` to select a bundle and `find_package(conflux REQUIRED
COMPONENTS ...)` to consume installed targets. The current component/target map
lives in [`docs/component-map.md`](docs/component-map.md); CI/package validation
commands live in [`docs/build-ci-lanes.md`](docs/build-ci-lanes.md).
