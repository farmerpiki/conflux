# Package Consumption

Conflux supports one public interface mode per configured package. Configure,
install, and consume header and module packages as separate lanes.

## Module Interface

`MODULE_INTERFACE` is the primary source-consumption and development mode for
the preview. It uses C++ module imports and remains toolchain-sensitive: use the
checked compiler/CMake presets, and expect strict configure failures when the
selected toolchain cannot provide the required module support.

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
not require CMake import-std discovery.

```sh
cmake -S . -B /tmp/conflux-header -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCONFLUX_USE_MOCK_LIBURING=ON \
  -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
  -DCONFLUX_BUILD_TESTS=OFF \
  -DCONFLUX_BUILD_BENCHMARKS=OFF \
  -DCONFLUX_ENABLE_DB=OFF
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

## Package Contract

- `find_package(conflux REQUIRED COMPONENTS ...)` imports only the requested
  components and their direct dependency closure. Unrequested package targets
  must not be visible to the consumer, so `find_package(conflux COMPONENTS dns)`
  may expose `conflux::dns`, `conflux::runtime`, `conflux::file_io`,
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
- mock-liburing install: `core`, `types`, `json`, `file_io_sync` only, as
  internal compile evidence for generated header artifacts.
- real-liburing install: `runtime` and `http` may be requested when producer
  configure found real `liburing` and consumers can find it through
  `pkg-config`.
- DB-off install: no generated DB headers and no `db` / `pg` package contract.
- DB-on install: `db` is available only when libpq headers/library are found.
- `MODULE_INTERFACE` is the prerelease primary interface.
- `HEADER_INTERFACE` exists for generated release artifacts and compatibility.

Runtime/http consumers use a real-liburing install:

```cmake
find_package(conflux REQUIRED COMPONENTS core json http runtime)
target_link_libraries(myapp PRIVATE conflux::core conflux::json conflux::http conflux::runtime)
```

Do not mix `import conflux.*` and generated Conflux headers in one consumer
package or executable. The package mode is selected with
`CONFLUX_INTERFACE_MODE` at configure time and reported by the installed package
config as `CONFLUX_INTERFACE_MODE`.

Keeping one public surface avoids duplicate API maintenance, ODR ambiguity,
package drift, and an unbounded consumer test matrix.
