# Package Consumption

Conflux supports one public interface mode per configured package.

`MODULE_INTERFACE` consumers use module imports:

```cpp
import conflux.json;
import conflux.http;
```

`HEADER_INTERFACE` consumers use generated headers:

```cpp
#include <conflux/json.hxx>
#include <conflux/net/http.hxx>
```

Do not mix `import conflux.*` and generated Conflux headers in one consumer
package or executable. The package mode is selected with
`CONFLUX_INTERFACE_MODE` at configure time and reported by the installed package
config as `CONFLUX_INTERFACE_MODE`.

Keeping one public surface avoids duplicate API maintenance, ODR ambiguity,
package drift, and an unbounded consumer test matrix.
