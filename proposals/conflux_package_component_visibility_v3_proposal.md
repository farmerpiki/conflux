# Proposal v3: Low-maintenance package component visibility

Status: implemented
Branch: `modules-first-release-artifacts`
state note: implemented package visibility design. This supersedes
`conflux_package_component_visibility_proposal.md` and
`conflux_package_component_visibility_counterproposal.md`.

Implemented state:

- split `confluxTargets-<component>.cmake` exports are the import boundary;
- `dns_bridge` is exported as `conflux::dns_bridge`;
- `cmake/conflux-config.cmake.in` uses generated metadata and generic DFS
  import logic, with no hand-written dependency table or component order;
- external dependencies are resolved from the requested closure rather than
  install-wide `CONFLUX_INSTALL_NEEDS_*` booleans;
- support targets are registered separately from requestable components;
- package smoke keeps requested components unchanged and includes negative
  visible-target/external-dependency checks;
- header mode treats `conflux_headers` as support metadata and keeps liburing on
  reached runtime/http components instead of the universal header target.

## Summary

`find_package(conflux REQUIRED COMPONENTS ...)` should expose only the requested
requestable components plus the internal and external dependency closure required
to use them. The closure must be derived from the installed CMake target exports,
not maintained as a second dependency graph in `conflux-config.cmake.in`.

The package keeps one installed export file per importable component:

```text
confluxTargets-core.cmake
confluxTargets-json.cmake
confluxTargets-dns.cmake
confluxTargets-http.cmake
...
```

The installed package config loads generated metadata, resolves only the
requested closure, resolves only the external packages needed by that closure,
and includes only the split export files for that closure.

## Decisions

- Split exports are the package visibility boundary.
- CMake target links are the source of truth for internal target dependencies.
- Installed export files are the source of truth consumed by the package
  metadata generator.
- Optional external dependencies are closure-scoped, not install-tree scoped.
- Support targets are dependency-visible only, not normal requested components.
- Requestable package components are not the same thing as first-contact docs.
  Fine-grained components such as `http_core` may remain requestable even if docs
  steer users to `http`.
- Public module import names stay short and stable through `.cppm` facades.

## Module Facade Contract

Public `.cppm` files are facades and should contain no implementation code:

```cpp
export module conflux.json;
export import :api;
export import :dom;
```

Where compiler support permits, former top-level implementation units become
partitions:

```cpp
export module conflux.json:api;
```

This preserves ergonomic public imports such as:

```cpp
import conflux.json;
import conflux.net.http;
```

Packaging should not introduce longer public module names such as
`conflux.package.json`.

## Component Model

Register installable targets with explicit kind:

```cmake
conflux_component(conflux_json json KIND REQUESTABLE)
conflux_component(conflux_net_http http KIND REQUESTABLE)
conflux_component(conflux_options _options KIND SUPPORT)
conflux_component(conflux_direct_slot_pool _direct_slot_pool KIND SUPPORT)
conflux_component(conflux_simd_runtime _simd_runtime KIND SUPPORT)
```

This can be implemented by extending the existing `conflux_public_component()`
registration with a `KIND` argument or by adding thin wrapper functions. The
important change is the registry shape, not the function name.

Recommended semantics:

- `conflux_AVAILABLE_COMPONENTS`: requestable components only.
- `conflux_AVAILABLE_TARGETS`: requestable targets only.
- `conflux_AVAILABLE_SUPPORT_TARGETS`: support targets available in the install
  tree.
- `conflux_VISIBLE_COMPONENTS`: requestable components imported by requested
  closure.
- `conflux_VISIBLE_TARGETS`: all imported targets, including support targets.
- `conflux_VISIBLE_SUPPORT_TARGETS`: imported support targets only.

Support targets should remain underscored only when they are true plumbing:

- `_options`
- `_direct_slot_pool`
- `_simd_runtime`

`dns_bridge` should be exported as `conflux::dns_bridge`, not
`conflux::_dns_bridge`, because it is a real installed provider edge used by the
HTTP client. It can remain absent from first-contact docs.

## Split Exports

Each registered component installs into its own export set:

```cmake
install(TARGETS conflux_json
    EXPORT confluxTargets-json
    ...)

install(EXPORT confluxTargets-json
    NAMESPACE conflux::
    FILE confluxTargets-json.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux)
```

Normal component imports must not include a monolithic `confluxTargets.cmake`.

`conflux::conflux` should be treated as an alias/compatibility entry for the
umbrella target. Keep the special case contained in metadata, not scattered
through package import logic.

## Generated Metadata

After split export files are installed, run an `install(SCRIPT ...)` generator
that writes:

```text
conflux-component-targets.cmake
conflux-component-deps.cmake
conflux-component-external-deps.cmake
```

The generator input is the component registry from configure time plus the
installed split export files.

Install-time path handling is part of the generator contract. The script must
write into the actual install tree used by `cmake --install`, including
`--prefix` overrides and `DESTDIR`. Do not bake the configure-time install
prefix into `file(WRITE ...)` paths.

### Internal Dependencies

For each `confluxTargets-<component>.cmake`, derive internal dependencies from
CMake's generated export dependency checks, specifically the block shaped like:

```cmake
foreach(_target "conflux::types" "conflux::runtime" ...)
```

This block is emitted by CMake for targets exported in other export sets and is
less fragile than scanning the whole export file. The generator should:

- collect only `conflux::...` entries from that dependency-check block;
- map each imported target back to a registered component;
- remove self references;
- remove duplicates;
- fail metadata generation for unknown `conflux::...` references;
- fail closed if the generated dependency-check block shape is not recognized;
- write deterministic output.

Keep parser fixtures for generated exports from the lowest supported CMake
version and from the current development CMake lane. This is still parsing
CMake-generated text, so export-format drift must break metadata generation
early instead of silently producing an incomplete graph.

Example generated internal metadata:

```cmake
set(_conflux_component_deps_json json_boundary _simd_runtime)
set(_conflux_component_deps_http_client dns_bridge http_core net_tls runtime types utils)
```

No hand-written `_conflux_component_deps_*` table should remain in source.

### External Dependencies

External dependency metadata is also closure-scoped. It may be generated by
parsing link-bearing properties in installed export files:

- `INTERFACE_LINK_LIBRARIES`
- `IMPORTED_CXX_MODULES_LINK_LIBRARIES`

The parser should recognize known provider targets and map them to dependency
tokens:

```text
PkgConfig::LIBURING           -> LIBURING
PkgConfig::XXHASH             -> XXHASH
PkgConfig::LIBPQ              -> LIBPQ
OpenSSL::SSL                  -> OPENSSL
OpenSSL::Crypto               -> OPENSSL
ZLIB::ZLIB                    -> ZLIB
PkgConfig::LIBDEFLATE         -> LIBDEFLATE
PkgConfig::ZLIB_NG            -> ZLIB_NG
PkgConfig::LIBISAL            -> LIBISAL
PkgConfig::BROTLI             -> BROTLI
PkgConfig::ZSTD               -> ZSTD
PkgConfig::NGHTTP2            -> NGHTTP2
PkgConfig::NGTCP2             -> NGTCP2
PkgConfig::NGTCP2_CRYPTO_OSSL -> NGTCP2_CRYPTO_OSSL
PkgConfig::NGHTTP3            -> NGHTTP3
PkgConfig::ARGON2             -> ARGON2
```

Normalize `$<LINK_ONLY:...>` before provider matching; static-library private
dependencies commonly appear that way in installed exports. Unknown external
provider targets and unsupported generator expressions in exported link
properties should fail metadata generation unless they are explicitly allowed as
plain system/library items. Known plain items such as `dl` should be allowed;
`pthread` and `Threads::Threads` should be handled explicitly if they appear.

Generated shape:

```cmake
set(_conflux_component_external_deps_json XXHASH)
set(_conflux_component_external_deps_runtime LIBURING)
set(_conflux_component_external_deps_pg LIBPQ)
set(_conflux_component_external_deps_http3 OPENSSL NGTCP2 NGTCP2_CRYPTO_OSSL NGHTTP3)
set(_conflux_component_external_deps_http_auth ARGON2)
```

Install-wide booleans may remain as capability-reporting variables, but they
must not drive unconditional `find_dependency()` or `pkg_check_modules()` calls.

## Generic Package Importer

`cmake/conflux-config.cmake.in` should contain generic logic only:

1. Include `CMakeFindDependencyMacro`.
2. Include generated component metadata files.
3. Default an empty component request to `core`.
4. Validate requested components against requestable components.
5. Recursively import dependency components first.
6. Detect cycles and report the full path.
7. Resolve external dependency tokens for each reached component exactly once.
8. Include `confluxTargets-<component>.cmake`.
9. Record visible requestable components and support targets.
10. Record resolved external dependency tokens.
11. Set `conflux_<component>_FOUND` for requested components.
12. Call `check_required_components(conflux)`.

There should be no component-specific dependency facts and no hand-written
global component order in `conflux-config.cmake.in`.

## External Dependency Resolver

The package config should resolve external tokens through one generic function:

```cmake
_conflux_find_external_dep(LIBURING)
_conflux_find_external_dep(OPENSSL)
```

Provider behavior:

- call `find_dependency(PkgConfig REQUIRED)` only before the first pkg-config
  token that needs it;
- call `pkg_check_modules(... REQUIRED IMPORTED_TARGET ...)` only for reached
  tokens;
- call `find_dependency(OpenSSL REQUIRED)` / `find_dependency(ZLIB REQUIRED)`
  only for reached tokens;
- call `pkg_check_modules(ARGON2 REQUIRED IMPORTED_TARGET libargon2)` only when
  a reached component exported `PkgConfig::ARGON2`;
- de-duplicate repeated tokens.

This must make a full install support:

```cmake
find_package(conflux REQUIRED COMPONENTS core)
```

without requiring unrelated optional libraries such as libpq, nghttp2, ngtcp2,
nghttp3, brotli, zstd, or compression backends.

## Header Mode

Header mode must use the same component names and visibility semantics.

If `conflux_headers` is needed, model it as a support component, not as an
unconditional dependency leak. Component targets should carry component-specific
external dependencies where possible. In particular, `core` should not require
liburing, libpq, HTTP protocol libraries, or compression libraries unless the
requested closure actually reaches components that need them.

Required behavior:

- `HEADER_INTERFACE` and `MODULE_INTERFACE` expose the same requestable component
  names when those components are built.
- Support/header bridge targets appear in `conflux_VISIBLE_SUPPORT_TARGETS` only
  when reached.
- Header mode package smokes should include at least core/json and one runtime or
  HTTP lane when supported by the feature set.

## Implementation Plan

### Phase 1: Clean Up Current Direction

1. Keep the existing split exports.
2. Keep the existing `dns_bridge` export rename.
3. Invert guards so `scripts/check-package-config.sh` rejects:
   - unconditional monolithic target inclusion;
   - in-template `_conflux_component_deps_*` tables;
   - in-template `_conflux_component_order`;
   - install-wide external dependency resolution for optional providers.
4. Add guards requiring generated component metadata files.
5. Remove unconditional support-target import when the generated importer lands.

### Phase 2: Component Registry

1. Replace `conflux_public_component(...)` with requestable/support registration,
   or extend it with a `KIND` argument.
2. Generate configured lists for:
   - requestable component names;
   - support component names;
   - target name by component;
   - export file by component;
   - alias metadata for `conflux::conflux` and `umbrella`.
3. Use those lists for split install/export loops.

### Phase 3: Metadata Generator

1. Add `cmake/ConfluxGeneratePackageMetadata.cmake.in`.
2. Configure it with the component registry.
3. Run it with `install(SCRIPT ...)` after all split export files are installed.
4. Generate target, internal dependency, and external dependency metadata.
5. Fail fast on missing export files, unknown internal targets, unknown external
   provider targets, malformed parse results, or cycles that cannot be reduced to
   self references.

### Phase 4: Generic Config Importer

1. Replace current component-specific package logic with generic DFS import.
2. Add closure-scoped external dependency resolver.
3. Preserve package mode variables:
   - `CONFLUX_INTERFACE_MODE`
   - `CONFLUX_PACKAGE_MOCK_LIBURING`
   - `CONFLUX_RUNTIME_MOCK`
4. Reclassify `CONFLUX_RUNTIME_REQUIRES_LIBURING` as install-tree capability or
   replace it with closure-visible external dependency reporting.
5. Remove the `CONFLUX_INSTALL_NEEDS_*` booleans as package import drivers.

### Phase 5: Smokes and Checks

1. Update package smoke summary:
   - requested components;
   - visible requestable components;
   - visible support targets;
   - visible targets;
   - resolved external dependency tokens;
   - interface mode.
2. Keep negative visible-target assertions.
3. Add negative external dependency assertions where practical.
4. Keep package smoke from auto-expanding requested components except in
   explicit closure tests.
5. Update `scripts/check-package-config.sh` to require generated metadata and
   reject monolithic unconditional target inclusion, in-template dependency
   tables, and install-wide optional dependency resolution.

## Acceptance Checks

Use tmpfs install-tree smokes and existing scripts. Do not pass explicit
parallelism flags.

Module interface:

```text
scripts/run-install-tree-smoke.sh --generator Ninja --feature-set core --interface-mode MODULE_INTERFACE --components core
scripts/run-install-tree-smoke.sh --generator Ninja --feature-set json --interface-mode MODULE_INTERFACE --components json
scripts/run-install-tree-smoke.sh --generator Ninja --feature-set web-server --interface-mode MODULE_INTERFACE --components template -- -DCONFLUX_USE_MOCK_LIBURING=OFF
scripts/run-install-tree-smoke.sh --generator Ninja --feature-set http-minimal --interface-mode MODULE_INTERFACE --components dns -- -DCONFLUX_USE_MOCK_LIBURING=OFF
scripts/run-install-tree-smoke.sh --generator Ninja --feature-set complete --interface-mode MODULE_INTERFACE --components pg --enable-db-smoke -- -DCONFLUX_USE_MOCK_LIBURING=OFF
scripts/run-install-tree-smoke.sh --generator Ninja --feature-set http-api --interface-mode MODULE_INTERFACE --components http -- -DCONFLUX_USE_MOCK_LIBURING=OFF
```

Header interface:

```text
scripts/run-install-tree-smoke.sh --generator Ninja --feature-set core --interface-mode HEADER_INTERFACE --components core
scripts/run-install-tree-smoke.sh --generator Ninja --feature-set json --interface-mode HEADER_INTERFACE --components json
```

Full-install negative visibility/dependency checks:

```text
full install, request only core
full install, request only json
full install, request only dns
full install, request only pg
full install, request only http
```

Expected outcomes:

- `core` does not expose `http`, `template`, `pg`, `db`, or HTTP protocol
  targets, and does not resolve unrelated optional external dependencies.
- `json` exposes only JSON closure and required support/external dependencies.
- `template` exposes template/json/file-sync closure, not HTTP/DNS/PG/DB.
- `dns` exposes DNS/runtime/file/socket/uring closure, not HTTP/template/PG/DB.
- `pg` exposes PG/runtime/file closure and resolves `LIBPQ`, but does not expose
  `db` unless `db` is requested.
- `http` exposes its real HTTP closure, including `dns_bridge` when needed, but
  not unrelated template or PG/DB components.

Then run:

```text
scripts/check-package-config.sh .
python3 scripts/check-package-docs.py
python3 scripts/check-release-docs.py
git diff --check
cmake --build --preset release-clang-libcxx
ctest --test-dir /tmp/gcc-16/release-clang-libcxx --output-on-failure -R '^(build/package-config|docs/package-docs|docs/release-docs)$'
```

## Documentation Updates

Update package docs to say:

- users request requestable package components;
- first-contact docs may recommend fewer high-level components;
- split exports are included lazily by dependency closure;
- support targets are dependency-visible only;
- external dependencies are resolved only when needed by the requested closure;
- there is no hand-maintained package dependency map;
- dependency changes belong in `target_link_libraries(...)`;
- new requestable components require registration and one isolated package smoke.

## Success Criteria

Implementation is complete when:

1. No hand-written component dependency graph remains.
2. No hand-written component import order remains.
3. No monolithic export file is included by default.
4. No support target is imported unconditionally.
5. No optional third-party package is required unless the requested closure needs
   it.
6. `find_package(conflux COMPONENTS core)` remains minimal even from a full
   install tree.
7. Future target or dependency changes require normal target links, component
   registration, and focused smoke coverage only.
