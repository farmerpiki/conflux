# Package Consumption

Conflux supports one public interface mode per configured package. Configure,
install, and consume header and module packages as separate lanes.

## Module Interface

`MODULE_INTERFACE` is the primary source-consumption and development mode for
the preview. It uses C++ module imports and remains toolchain-sensitive: use the
checked compiler/CMake presets, and expect strict configure failures when the
selected toolchain cannot provide the required module support. The release
support contract is intentionally narrow: GCC 15, GCC 16, and Clang 21 with
CMake 4.2+ and Ninja unless a release evidence manifest says otherwise.

```cpp
import conflux.json;
import conflux.http;
```

```cmake
find_package(conflux REQUIRED COMPONENTS core json http)
target_link_libraries(myapp PRIVATE conflux::core conflux::json conflux::http)
```

## Generated Header Artifact

Generated headers are release artifacts for consumers that cannot use modules.
They are generated from the module source, shipped in staged release artifacts,
and are not the design center for new API work. Header-interface packages should
not require CMake import-std discovery. Header mode is supported only for the
installed components and toolchains covered by the matching package smoke; other
uses are best-effort compatibility.

```sh
cmake -S . -B /tmp/conflux-header -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
  -DCONFLUX_BUILD_TESTS=OFF \
  -DCONFLUX_BUILD_BENCHMARKS=OFF \
  -DCONFLUX_POSTGRES_PROVIDER=OFF
cmake --build /tmp/conflux-header
cmake --install /tmp/conflux-header --prefix /tmp/conflux-install
```

Consumers include generated headers:

```cpp
#include <conflux/json.hxx>
#include <conflux/file_io_sync.hxx>
```

```cmake
find_package(conflux REQUIRED COMPONENTS core json file_io_sync)
target_link_libraries(myapp PRIVATE conflux::core conflux::json conflux::file_io_sync)
```


## CPU / ISA Dispatch for Distribution Packages

Distribution packages should build with a portable compiler baseline and enable
`CONFLUX_SIMD_SELECTION=RUNTIME`.
This allows Conflux to compile optional ISA-specific objects such as
AES-NI/PCLMUL or AVX2 SIMD scan helpers while selecting supported scan helpers
at runtime. On Linux x86 Clang/GCC builds, JSON stdsimd scan helpers use ELF
IFUNC and resolve once to AVX2, SSE2, or scalar code. Other runtime builds keep
guarded call-site fallback. The generated binary remains runnable on machines
that lack those features and falls back to scalar code.

`CONFLUX_SIMD_SELECTION=DIRECT` removes runtime AVX2 probes from selected SIMD
scan call sites. While the stdsimd objects are built with `-mavx2`, direct
`STDX`/`STD26` builds are AVX2-specific and must only be shipped when package
metadata declares that CPU baseline. `CONFLUX_SIMD_SELECTION=AUTO` resolves to
`DIRECT` and is intended for local appliance and benchmark builds where every
deployment target is known to support the selected ISA-specific objects.

Do not ship packages built with `-march=native` or unconditional deployment ISA
flags unless the package metadata declares that CPU baseline.

## Package Contract

- `find_package(conflux REQUIRED COMPONENTS ...)` imports only the requested
  components and their direct dependency closure. Unrequested package targets
  must not be visible to the consumer, so `find_package(conflux COMPONENTS dns)`
  may expose `conflux::dns`, `conflux::work`, `conflux::file_io`,
  `conflux::socket_io`, `conflux::uring`, and other required support targets,
  but it must not expose unrelated targets such as `conflux::http`,
  `conflux::template`, or `conflux::pg`.
- Installed CMake exports are split by component as
  `confluxTargets-<component>.cmake`. Register new public components in
  `CMakeLists.txt` and express component/provider dependencies with
  `target_link_libraries`; installed package metadata is generated from the
  exported targets.
- Package smoke tests configure a downstream consumer and fail if any installed
  public target exists outside `conflux_VISIBLE_TARGETS`. Check
  isolated components with `scripts/run-install-tree-smoke.sh --components
  '<component>' --feature-set <matching-feature-set>`; this is the guard that
  prevents broad aggregates from leaking into minimal installs.
- liburing-free install: `core`, `types`, `json`, `file_io_sync` only.
- real-liburing install: `work` and `http` may be requested when producer
  configure found real `liburing` and consumers can find it through
  `pkg-config`.
- DB-off install: no generated DB headers and no `db` / `pg` package contract.
- DB-on install: `db` is available only when libpq headers/library are found.
- `MODULE_INTERFACE` is the prerelease primary interface.
- `HEADER_INTERFACE` exists for generated release artifacts and compatibility.

Runtime/http consumers use a real-liburing install:

```cmake
find_package(conflux REQUIRED COMPONENTS core json http work)
target_link_libraries(myapp PRIVATE conflux::core conflux::json conflux::http conflux::work)
```

Do not mix `import conflux.*` and generated Conflux headers in one consumer
package or executable. The package mode is selected with
`CONFLUX_INTERFACE_MODE` at configure time and reported by the installed package
config as `CONFLUX_INTERFACE_MODE`.
Mixed import/include package smoke is an internal drift and ODR guard; external
consumers should still choose one interface mode per target/package.

Keeping one public surface avoids duplicate API maintenance, ODR ambiguity,
package drift, and an unbounded consumer test matrix.
