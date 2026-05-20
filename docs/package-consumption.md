# Package Consumption

Conflux supports one public interface mode per configured package. Configure,
install, and consume header and module packages as separate lanes.

## Header Interface

Header-interface packages are the prerelease consumption baseline. They should
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
#include <conflux/net/http.hxx>
```

```cmake
find_package(conflux REQUIRED COMPONENTS core json http)
target_link_libraries(myapp PRIVATE conflux::core conflux::json conflux::http)
```

## Module Interface

Module-interface packages use C++ module imports and remain toolchain-sensitive.
They may require CMake import-std support for the selected compiler and standard
library.

```cpp
import conflux.json;
import conflux.http;
```

```cmake
find_package(conflux REQUIRED COMPONENTS core json http)
target_link_libraries(myapp PRIVATE conflux::core conflux::json conflux::http)
```

Do not mix `import conflux.*` and generated Conflux headers in one consumer
package or executable. The package mode is selected with
`CONFLUX_INTERFACE_MODE` at configure time and reported by the installed package
config as `CONFLUX_INTERFACE_MODE`.

Keeping one public surface avoids duplicate API maintenance, ODR ambiguity,
package drift, and an unbounded consumer test matrix.
