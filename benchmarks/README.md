# Benchmarks

`conflux_benchmarks` is a dedicated benchmark target for focused, repeatable
in-process performance work. It is intentionally scoped to routing, header
lookup, middleware composition, and compression paths, so the numbers are not
polluted by sandbox/kernel/io_uring variability.

Build:

```sh
cmake --preset release-clang-libcxx
cmake --build --preset release-clang-libcxx --target conflux_benchmarks -j1
```

or:

```sh
cmake --preset release-gcc-stdcxx
cmake --build --preset release-gcc-stdcxx --target conflux_benchmarks -j1
```

Benchmarks are also available in the debug presets when you want easier
instrumentation or debugger access:

```sh
cmake --preset debug-clang-libcxx
cmake --build --preset debug-clang-libcxx --target conflux_benchmarks -j1
```

Run:

```sh
./build/release-clang-libcxx/benchmarks/conflux_benchmarks
./build/release-clang-libcxx/benchmarks/conflux_benchmarks --list
./build/release-clang-libcxx/benchmarks/conflux_benchmarks --filter router
./build/release-clang-libcxx/benchmarks/conflux_benchmarks --iterations 50000
./build/release-clang-libcxx/benchmarks/conflux_benchmarks --format csv
```

Current benchmark groups:

- `micro/*`: small hot-path operations
- `flow/*`: full in-process request flows through the public router/middleware API

This suite is the intended baseline before HTTP client work. If network or
io_uring transport benchmarks are needed later, they should be added as
separate cases rather than mixed into these logic-path measurements.
