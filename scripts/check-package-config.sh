#!/usr/bin/env bash
set -euo pipefail

root="${1:-$(pwd)}"
cd "$root"

fail() {
    printf 'check-package-config: %s\n' "$*" >&2
    exit 1
}

[[ -f CMakeLists.txt ]] || fail "missing CMakeLists.txt"
[[ -f cmake/conflux-config.cmake.in ]] || fail "missing package config template"
[[ -f cmake/ConfluxOptions.cmake ]] || fail "missing option/normalization CMake module"
[[ -f cmake/ConfluxGeneratePackageMetadata.cmake.in ]] || fail "missing package metadata generator"
[[ -f cmake/ConfluxInstall.cmake ]] || fail "missing install/export CMake module"
[[ -f cmake/package-smoke/CMakeLists.txt ]] || fail "missing package smoke project"
[[ -f scripts/run-package-config-smoke.sh ]] || fail "missing package smoke runner"
[[ -f scripts/run-install-tree-smoke.sh ]] || fail "missing install-tree smoke runner"
[[ -f scripts/check-package-smoke-liburing-free.sh ]] || fail "missing liburing-free package smoke lane"
[[ -f scripts/check-package-smoke-core-isolated.sh ]] || fail "missing core-isolated package smoke lane"
[[ -f scripts/check-package-smoke-runtime.sh ]] || fail "missing runtime package smoke lane"
[[ -f scripts/check-package-smoke-db.sh ]] || fail "missing DB package smoke lane"
[[ -f scripts/check-cmake-source-files.py ]] || fail "missing CMake source-file guard"
[[ -f scripts/check-component-map.py ]] || fail "missing component-map guard"
[[ -f scripts/check-planning-state.py ]] || fail "missing planning-state guard"
[[ -f scripts/check-release-docs.py ]] || fail "missing release-docs guard"
[[ -f scripts/check-package-docs.py ]] || fail "missing package-docs guard"
[[ -f scripts/check-release-notes.py ]] || fail "missing release-notes guard"
[[ -f scripts/stage-release-artifacts.sh ]] || fail "missing release artifact staging script"
[[ -f scripts/check-release-artifact.py ]] || fail "missing release artifact guard"

grep -Eq '^project\(conflux VERSION [0-9]+\.[0-9]+\.[0-9]+ LANGUAGES CXX\)' CMakeLists.txt \
    || fail "project() must declare the package version"

grep -q 'configure_package_config_file(' cmake/ConfluxInstall.cmake \
    || fail "missing configure_package_config_file()"
grep -q 'write_basic_package_version_file(' cmake/ConfluxInstall.cmake \
    || fail "missing write_basic_package_version_file()"
grep -q 'VERSION ${PROJECT_VERSION}' cmake/ConfluxInstall.cmake \
    || fail "package version file must use PROJECT_VERSION"
grep -q 'install(EXPORT confluxTargets' cmake/ConfluxInstall.cmake \
    || fail "missing install(EXPORT confluxTargets)"
grep -q 'install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/ConfluxGeneratePackageMetadata.cmake")' cmake/ConfluxInstall.cmake \
    || fail "missing install-time package metadata generator"
grep -q 'NAMESPACE conflux::' cmake/ConfluxInstall.cmake \
    || fail "export namespace must stay conflux::"
grep -q 'add_test(NAME build/cmake-source-files' tests/CMakeLists.txt \
    || fail "missing CMake source-file CTest guard"
grep -q 'add_test(NAME build/component-map' tests/CMakeLists.txt \
    || fail "missing component-map CTest guard"
grep -q 'add_test(NAME build/package-config' tests/CMakeLists.txt \
    || fail "missing package-config CTest guard"
grep -q 'add_test(NAME docs/planning-state' tests/CMakeLists.txt \
    || fail "missing planning-state CTest guard"
grep -q 'add_test(NAME docs/release-docs' tests/CMakeLists.txt \
    || fail "missing release-docs CTest guard"
grep -q 'add_test(NAME docs/package-docs' tests/CMakeLists.txt \
    || fail "missing package-docs CTest guard"
grep -q 'add_test(NAME docs/release-notes' tests/CMakeLists.txt \
    || fail "missing release-notes CTest guard"
grep -q 'CONFLUX_PACKAGE_SMOKE_COMPONENTS' cmake/ConfluxOptions.cmake \
    || fail "missing package smoke component cache variable"
grep -q 'add_test(NAME build/package-config-install-tree' tests/CMakeLists.txt \
    || fail "missing installed-prefix package smoke CTest guard"
grep -q 'CONFLUX_BUILD_PACKAGE_TESTS' cmake/ConfluxOptions.cmake \
    || fail "missing package-only CTest option"
grep -q 'CONFLUX_HEADER_FAST_COMPILE' cmake/ConfluxOptions.cmake \
    || fail "missing header fast-compile option"
grep -q 'CONFLUX_HEADER_LINK_EXAMPLES' cmake/ConfluxOptions.cmake \
    || fail "missing opt-in linked header examples option"
grep -q 'CONFLUX_HEADER_LINK_SMOKE' cmake/ConfluxOptions.cmake \
    || fail "missing opt-in linked header smoke option"
grep -q 'CXX_SCAN_FOR_MODULES OFF' cmake/ConfluxInterfaceMode.cmake \
    || fail "header generated targets must disable module scanning"
grep -q 'CONFLUX_HEADER_FAST_COMPILE' cmake/ConfluxInterfaceMode.cmake \
    || fail "header generated targets must honor fast-compile option"
grep -q 'CONFLUX_HEADER_LINK_EXAMPLES' cmake/ConfluxInterfaceMode.cmake \
    || fail "header examples must keep implementation linking opt-in"
grep -q 'conflux_add_header_link_smoke_targets' cmake/ConfluxInterfaceMode.cmake \
    || fail "header mode must expose a linked smoke target"
grep -q 'header/link-smoke-http' cmake/ConfluxInterfaceMode.cmake \
    || fail "header linked HTTP smoke must be registered with CTest"
grep -q 'conflux_header_impl_json' cmake/ConfluxInterfaceMode.cmake \
    || fail "header implementation sources must be split by component"
grep -q 'COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-O0' cmake/ConfluxInterfaceMode.cmake \
    || fail "header generated targets must override release optimization for fast compile"
grep -q 'CONFLUX_RUN_INSTALL_TREE_SMOKE' cmake/ConfluxOptions.cmake \
    || fail "missing opt-in install-tree smoke CTest option"
grep -q 'add_test(NAME build/install-tree-smoke' tests/CMakeLists.txt \
	|| fail "missing install-tree smoke CTest guard"
grep -q 'set(CMAKE_CXX_SCAN_FOR_MODULES OFF)' cmake/ConfluxOptions.cmake \
	|| fail "HEADER_INTERFACE must disable CMake module scanning"
grep -q '"name": "release-header-artifacts"' CMakePresets.json \
    || fail "missing release-header-artifacts preset"
grep -q '"configurePreset": "release-clang-libcxx"' CMakePresets.json \
    || fail "missing release-clang-libcxx test preset"
grep -q '"name": "release-core-install-smoke"' CMakePresets.json \
    || fail "missing release-core-install-smoke preset"
grep -q '"name": "release-json-install-smoke"' CMakePresets.json \
    || fail "missing release-json-install-smoke preset"
grep -q '"name": "release-header-artifacts-install-smoke"' CMakePresets.json \
    || fail "missing release-header-artifacts-install-smoke preset"

grep -q '@PACKAGE_INIT@' cmake/conflux-config.cmake.in \
    || fail "package config must use PACKAGE_INIT"
grep -q 'include(CMakeFindDependencyMacro)' cmake/conflux-config.cmake.in \
    || fail "package config must include CMakeFindDependencyMacro"
grep -q 'conflux-component-targets.cmake' cmake/conflux-config.cmake.in \
    || fail "package config must include generated component target metadata"
grep -q 'conflux-component-deps.cmake' cmake/conflux-config.cmake.in \
    || fail "package config must include generated component dependency metadata"
grep -q 'conflux-component-external-deps.cmake' cmake/conflux-config.cmake.in \
    || fail "package config must include generated external dependency metadata"
grep -q 'confluxTargets-${_export_component}.cmake' cmake/conflux-config.cmake.in \
    || fail "package config must include requested split exported targets"
grep -q 'set(conflux_AVAILABLE_COMPONENTS' cmake/conflux-config.cmake.in \
    || fail "package config must expose available components"
grep -q 'set(conflux_AVAILABLE_SUPPORT_TARGETS' cmake/conflux-config.cmake.in \
    || fail "package config must expose available support targets"
grep -q 'set(conflux_VISIBLE_COMPONENTS' cmake/conflux-config.cmake.in \
    || fail "package config must expose visible components"
grep -q 'set(conflux_VISIBLE_SUPPORT_TARGETS' cmake/conflux-config.cmake.in \
    || fail "package config must expose visible support targets"
grep -q 'set(conflux_RESOLVED_EXTERNAL_DEPS' cmake/conflux-config.cmake.in \
    || fail "package config must expose resolved external deps"
grep -q '_conflux_import_component' cmake/conflux-config.cmake.in \
    || fail "package config must compute requested component dependency closure"
grep -q '_conflux_find_external_dep' cmake/conflux-config.cmake.in \
    || fail "package config must resolve closure-scoped external deps"
if grep -q '^set(_conflux_component_deps_' cmake/conflux-config.cmake.in; then
    fail "package config must not contain a hand-written component dependency table"
fi
if grep -q '^set(_conflux_component_order' cmake/conflux-config.cmake.in; then
    fail "package config must not contain a hand-written component import order"
fi
if grep -q '@CONFLUX_INSTALL_NEEDS_.*pkg_check_modules\|if(@CONFLUX_INSTALL_NEEDS_' cmake/conflux-config.cmake.in; then
    fail "package config must not resolve optional deps from install-wide booleans"
fi
if grep -q 'target_link_libraries *( *conflux_headers .*PkgConfig::LIBURING' cmake/ConfluxInterfaceMode.cmake; then
    fail "header support target must not leak liburing into every header package component"
fi
if grep -q 'target_link_libraries *( *conflux_headers .*PkgConfig::XXHASH' cmake/ConfluxInterfaceMode.cmake; then
    fail "header support target must not leak xxhash into every header package component"
fi
grep -q 'set(CONFLUX_RUNTIME_REQUIRES_LIBURING' cmake/conflux-config.cmake.in \
    || fail "package config must expose runtime liburing status"
grep -q 'set(CONFLUX_PACKAGE_MOCK_LIBURING' cmake/conflux-config.cmake.in \
    || fail "package config must expose mock-liburing producer status"
grep -q 'set(CONFLUX_RUNTIME_MOCK' cmake/conflux-config.cmake.in \
    || fail "package config must expose runtime mock status"
grep -q 'foreach(_conflux_component IN LISTS conflux_FIND_COMPONENTS)' cmake/conflux-config.cmake.in \
    || fail "package config must validate requested components"
grep -q 'check_required_components(conflux)' cmake/conflux-config.cmake.in \
    || fail "package config must call check_required_components(conflux)"
grep -q 'conflux::conflux' cmake/conflux-config.cmake.in \
    || fail "package config must provide the canonical umbrella alias when available"

grep -q 'find_package(conflux REQUIRED COMPONENTS' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke project must consume find_package(conflux COMPONENTS ...)"
grep -q 'add_executable(conflux_package_smoke "${_conflux_package_smoke_source}")' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke project must compile a generated downstream executable"
grep -q 'target_link_libraries(conflux_package_smoke PRIVATE' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke project must link installed namespaced targets"
grep -q 'add_test(NAME package-smoke/run' cmake/package-smoke/CMakeLists.txt \
	|| fail "package smoke project must run the downstream executable"
grep -q 'CONFLUX_PACKAGE_SMOKE_EXERCISE_LINKED_APIS' cmake/package-smoke/CMakeLists.txt \
	|| fail "package smoke must distinguish declaration-only and linked API lanes"
grep -q 'conflux::json::parse' cmake/package-smoke/CMakeLists.txt \
	|| fail "package smoke must exercise installed JSON implementation symbols when available"
grep -q 'conflux::build_info_summary' cmake/package-smoke/CMakeLists.txt \
	|| fail "package smoke must exercise installed core implementation symbols when available"
grep -q 'read_text_file_nothrow' cmake/package-smoke/CMakeLists.txt \
	|| fail "package smoke must exercise installed file_io_sync implementation symbols when available"
grep -q 'CONFLUX_HAS_JSON' cmake/package-smoke/CMakeLists.txt \
	|| fail "package smoke must assert installed JSON feature macros"
grep -q 'set(CMAKE_CXX_SCAN_FOR_MODULES OFF)' cmake/package-smoke/CMakeLists.txt \
	|| fail "header package smoke must disable module scanning"
grep -q 'set(CMAKE_CXX_SCAN_FOR_MODULES ON)' cmake/package-smoke/CMakeLists.txt \
	|| fail "module package smoke must enable module scanning"
grep -q 'import conflux.types;' cmake/package-smoke/CMakeLists.txt \
	|| fail "module package smoke source must import an installed conflux module"
grep -q '#include <conflux/types.hpp>' cmake/package-smoke/CMakeLists.txt \
    || fail "header package smoke source must include only the installed core header"
grep -q 'available_components=${conflux_AVAILABLE_COMPONENTS}' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke summary must report available components"
grep -q 'visible_components=${conflux_VISIBLE_COMPONENTS}' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke summary must report visible components"
grep -q 'visible_support_targets=${conflux_VISIBLE_SUPPORT_TARGETS}' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke summary must report visible support targets"
grep -q 'resolved_external_deps=${conflux_RESOLVED_EXTERNAL_DEPS}' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke summary must report resolved external deps"
grep -q 'CONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke must support negative external dependency assertions"
grep -q 'CONFLUX_PACKAGE_SMOKE_FAST_COMPILE' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke must expose a fast-compile option"
grep -q 'CONFLUX_PACKAGE_SMOKE_ENABLE_IMPORT_STD' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke must make import-std experimental support opt-in"
grep -q 'CONFLUX_PACKAGE_SMOKE_API_SURFACE' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke must expose an expected API-surface assertion"
grep -q 'CONFLUX_API_SURFACE_LEVEL != CONFLUX_API_SURFACE_' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke must compile-check installed API-surface macros"
grep -q 'expected_api_surface=${CONFLUX_PACKAGE_SMOKE_API_SURFACE}' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke summary must report expected API surface"
grep -q 'conflux_apply_package_smoke_build_policy' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke targets must apply fast-compile policy"
grep -q 'CXX_SCAN_FOR_MODULES OFF' cmake/package-smoke/CMakeLists.txt \
    || fail "header package smoke must disable module scanning"
grep -q -- '--forbid-external-deps' scripts/run-package-config-smoke.sh \
    || fail "package smoke runner must expose negative external dependency assertions"
grep -q -- '--api-surface' scripts/run-package-config-smoke.sh \
    || fail "package smoke runner must expose API-surface assertions"
grep -q -- '--enable-import-std' scripts/run-package-config-smoke.sh \
    || fail "package smoke runner must expose import-std opt-in"
grep -q 'found unrequested visible target' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke must reject unrequested visible targets"
grep -q 'runtime_requires_liburing=${CONFLUX_RUNTIME_REQUIRES_LIBURING}' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke summary must report runtime/liburing status"
grep -q 'package_mock_liburing=${CONFLUX_PACKAGE_MOCK_LIBURING}' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke summary must report producer mock-liburing status"
grep -q 'cmake --build "\$build_dir"' scripts/run-package-config-smoke.sh \
    || fail "package smoke runner must build the downstream project"
grep -q 'ctest --test-dir "\$build_dir" --output-on-failure' scripts/run-package-config-smoke.sh \
    || fail "package smoke runner must run downstream CTest"
grep -q 'conflux-package-smoke-summary.txt' scripts/run-package-config-smoke.sh \
    || fail "package smoke runner must print the downstream summary"
grep -q -- '--enable-db' scripts/run-package-config-smoke.sh \
    || fail "package smoke runner must expose a DB-enabled smoke option"
grep -q 'cmake --build "\$build_dir" --target install' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must build and install conflux"
grep -q -- '--interface-mode' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must forward interface mode"
grep -q -- '--api-surface' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must forward API surface"
grep -q -- '--enable-import-std-smoke' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must forward import-std smoke opt-in"
grep -q -- '--generator' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must forward generator"
grep -q 'extra_cmake_args' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must forward extra configure args"
grep -q 'run-package-config-smoke.sh' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must consume the installed prefix"
grep -q -- '--enable-db-smoke' scripts/run-install-tree-smoke.sh \
	|| fail "install-tree smoke runner must forward DB-enabled package smoke"
grep -q -- '--forbid-components' scripts/run-install-tree-smoke.sh \
	|| fail "install-tree smoke runner must forward forbidden component assertions"
grep -q -- '--forbid-external-deps' scripts/run-install-tree-smoke.sh \
	|| fail "install-tree smoke runner must forward forbidden external dependency assertions"
grep -q "core;json;file_io_sync" scripts/check-package-smoke-liburing-free.sh \
	|| fail "liburing-free package smoke must request only liburing-free components"
grep -q "LIBURING;LIBPQ;OPENSSL" scripts/check-package-smoke-liburing-free.sh \
	|| fail "liburing-free package smoke must explicitly forbid runtime/db/tls external deps"
grep -q "CONFLUX_JSON_HASH_PROVIDER=XXHASH" scripts/check-package-smoke-core-isolated.sh \
    || fail "core-isolated package smoke must force the external JSON hash provider"
grep -q -- "--components core" scripts/check-package-smoke-core-isolated.sh \
    || fail "core-isolated package smoke must request only core"
grep -q "core;json;http;file_io_sync;work" scripts/check-package-smoke-runtime.sh \
    || fail "runtime package smoke must request work/http components"
grep -q "pkg-config --exists liburing" scripts/check-package-smoke-runtime.sh \
    || fail "runtime package smoke must gate on real liburing"
grep -q "pkg-config --exists libpq" scripts/check-package-smoke-db.sh \
    || fail "DB package smoke must gate on libpq"
grep -q -- "--enable-db-smoke" scripts/check-package-smoke-db.sh \
    || fail "DB package smoke must enable DB component checks"
grep -q 'check-release-artifact.py' scripts/stage-release-artifacts.sh \
    || fail "release artifact staging must self-check staged output"
grep -q 'cmake --preset "$preset"' scripts/stage-release-artifacts.sh \
    || fail "release artifact staging must prefer the preset build"
grep -q 'module-header-bridge-manifest.json' scripts/stage-release-artifacts.sh \
    || fail "release artifact staging must include the bridge manifest"
grep -q 'python_version' scripts/check-release-artifact.py \
    || fail "release artifact guard must validate bridge python metadata"

printf 'check-package-config: ok\n'
