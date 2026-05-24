# API surface profiles

`CONFLUX_API_SURFACE` controls only the aggregate API re-exported by
`import conflux;` and `<conflux.hxx>`. It does not select components, link
optional providers, install extra packages, or hide built leaf modules from
explicit imports.

```sh
-DCONFLUX_API_SURFACE=curated
-DCONFLUX_API_SURFACE=extended
-DCONFLUX_API_SURFACE=complete
```

Default: `curated`.

## Profiles

| Profile | Entry points | Intended use |
|---|---|---|
| `curated` | `import conflux;`, `import conflux.curated;`, `<conflux.hxx>`, `<conflux/curated.hxx>` | recommended first-contact app/library surface, including the HTTP façade when built |
| `extended` | `import conflux.extended;`, `<conflux/extended.hxx>` | stable extension points and production customization knobs |
| `complete` | `import conflux.complete;`, `<conflux/complete.hxx>` | documented low-level escape hatches |

The profile hierarchy is additive: `curated` is a subset of `extended`, and
`extended` is a subset of `complete`. A higher profile should not replace the
handler model or JSON model from a lower profile.

## Feature sets remain separate

`CONFLUX_FEATURE_SET` and `CONFLUX_BUILD_*` decide which components are built.
`CONFLUX_API_SURFACE` decides only how much of the built, documented public API
is re-exported by the selected aggregate.

Examples:

```sh
# Full HTTP-capable build, but a narrow first-contact aggregate.
cmake -S . -B build \
  -DCONFLUX_FEATURE_SET=http-api \
  -DCONFLUX_API_SURFACE=curated

# Same build, broad aggregate for migration or low-level examples.
cmake -S . -B build-complete \
  -DCONFLUX_FEATURE_SET=http-api \
  -DCONFLUX_API_SURFACE=complete
```

Direct leaf imports remain valid whenever their component is built:

```cpp
import conflux.uring;      // explicit low-level import
import conflux.json;       // explicit JSON import
import conflux.http;       // explicit HTTP app façade
```

## Stage 1 availability

The aggregate `conflux` target is currently available for feature sets that
build the HTTP aggregate target. Core-only and JSON-only builds should continue
to use explicit leaf imports such as `conflux.types`, `conflux.json`, or
`conflux.file_io_sync` until a deliberately narrow aggregate target is added.

Header-mode generation may physically emit `<conflux/curated.hxx>`,
`<conflux/extended.hxx>`, and `<conflux/complete.hxx>` even in narrower builds.
Those files are supported only when the package also installs/smokes the
corresponding aggregate surface for the configured feature set.

## Profile contents

| Surface area | Curated | Extended | Complete |
|---|---:|---:|---:|
| `conflux.features` | yes | yes | yes |
| `conflux.types` | no, pending alias policy | no, pending alias policy | no, pending alias policy |
| `conflux.http` façade | yes, when built | yes | yes |
| `conflux.json` normal API | yes, when built | yes | yes |
| JSON boundary/provider/reflect modules | no | yes, when built | yes |
| `conflux.work` runtime/task primitives | no | yes, when built | yes |
| sync file helpers and mapped-file helpers | no | yes, when built | yes |
| auth/policy/observability/OpenAPI handler/vhost customization | no | yes, when built | yes |
| raw `io_uring`, socket/file async I/O, protocol/parser/router internals | no | no | yes, when built |
| private detail modules, partitions, tests, benches, generated glue | no | no | no |

`complete` means complete documented public surface, not private internals.

## Migration notes

Before API-surface profiles, `import conflux;` behaved like a broad aggregate.
Code that relied on that behavior should migrate to one of these shapes:

```cpp
import conflux.complete; // closest migration path for documented low-level use
```

or, preferably, explicit leaf imports:

```cpp
import conflux.uring;
import conflux.work;
import conflux.file_io;
```

Code using shorthand aliases from `conflux.types` should import `conflux.types`
explicitly until the public alias policy is finalized.

## Build metadata

`conflux::build_info().api_surface` reports the selected CMake value.
`conflux::API_SURFACE` and `conflux::API_SURFACE_LEVEL` expose the same setting
from `conflux.features`.


## HTTP façade split

`conflux.http` is the curated HTTP application façade and is re-exported by the
curated profile when the HTTP aggregate target is built. Explicit
`import conflux.http;` remains valid for users who want the leaf façade without
the selected aggregate profile.

`conflux.http` keeps route registration,
request/response helpers, typed extractors, JSON response helpers, common middleware
helper functions, and coroutine handler spelling. Lower-level WorkPool-based offload helpers,
blocking file helpers, OpenAPI route-handler mounting, named middleware concept aliases,
and raw-router access are intentionally outside the curated façade; use `import conflux.http.extended`,
`import conflux.extended`, or explicit leaf imports for those escape hatches.
