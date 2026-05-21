# Module fragility guardrails

GCC 16 module builds have hit CMI deserialization failures when large exported
module interfaces directly carry coroutine-heavy implementation detail. The
current project mitigation is intentionally boring: keep fragile exported module
interfaces thin, put coroutine bodies and synchronization state into normal
implementation units, and keep a targeted `-fno-module-lazy` fallback where GCC
16 still needs it.

The default GCC build uses lazy module imports. Standalone example executables
still opt back into eager module imports because GCC 16 can otherwise fail while
instantiating inline standard-library bodies from the lazy-loaded `std` module.

## Current protected patterns

`conflux.net.cancel` is the reference pattern:

- `src/net/cancel.cxx` exports declarations only.
- It does not use a global module fragment, textual includes, `import std`,
  coroutine bodies, mutexes, atomics, or optional state.
- `src/net/cancel_impl.cxx` is a private implementation unit with the textual
  headers, synchronization state, and coroutine bodies.
- `src/net/cancel_impl.cxx` must stay a `PRIVATE` source, not a public
  `CXX_MODULES` file-set entry.

`conflux.socket_io.coro` is still a larger exported interface, but it has one
important build-stability rule until it is split further: use explicit standard
headers in its global module fragment and do not reintroduce `import std` or
`import std.compat` into `src/socket_io/socket_io_coro.cxx`. The exported API
still names `std::span`, `std::shared_ptr`, `std::vector`, and related types, so
the standard declarations are needed, but importing the `std` module there makes
GCC deserialize that module through a large public socket/coroutine CMI. A
`debug-gcc16-stdcxx` build of `conflux_socket_io` was verified with the explicit
header shape. Method bodies that are already in
`src/socket_io/socket_io_coro_impl.cxx` must stay in that private implementation
unit.

## Regression check

Run:

```sh
scripts/check-module-interface-regressions.sh
```

CTest also exposes this as:

```sh
ctest --test-dir <build-dir> -R build/module-fragility-regression --output-on-failure
```

This is a source-shape guardrail, not a substitute for compiler coverage. Its job
is to catch the common accidental regressions before a GCC module build fails
with opaque CMI diagnostics.
