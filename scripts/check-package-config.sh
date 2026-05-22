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
[[ -f cmake/ConfluxGeneratePackageMetadata.cmake.in ]] || fail "missing package metadata generator"
[[ -f cmake/package-smoke/CMakeLists.txt ]] || fail "missing package smoke project"
[[ -f scripts/run-package-config-smoke.sh ]] || fail "missing package smoke runner"
[[ -f scripts/run-install-tree-smoke.sh ]] || fail "missing install-tree smoke runner"
[[ -f scripts/check-package-smoke-liburing-free.sh ]] || fail "missing liburing-free package smoke lane"
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

grep -q 'configure_package_config_file(' CMakeLists.txt \
    || fail "missing configure_package_config_file()"
grep -q 'write_basic_package_version_file(' CMakeLists.txt \
    || fail "missing write_basic_package_version_file()"
grep -q 'VERSION ${PROJECT_VERSION}' CMakeLists.txt \
    || fail "package version file must use PROJECT_VERSION"
grep -q 'install(EXPORT confluxTargets' CMakeLists.txt \
    || fail "missing install(EXPORT confluxTargets)"
grep -q 'install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/ConfluxGeneratePackageMetadata.cmake")' CMakeLists.txt \
    || fail "missing install-time package metadata generator"
grep -q 'NAMESPACE conflux::' CMakeLists.txt \
    || fail "export namespace must stay conflux::"
grep -q 'add_test(NAME build/cmake-source-files' CMakeLists.txt \
    || fail "missing CMake source-file CTest guard"
grep -q 'add_test(NAME build/component-map' CMakeLists.txt \
    || fail "missing component-map CTest guard"
grep -q 'add_test(NAME build/package-config' CMakeLists.txt \
    || fail "missing package-config CTest guard"
grep -q 'add_test(NAME docs/planning-state' CMakeLists.txt \
    || fail "missing planning-state CTest guard"
grep -q 'add_test(NAME docs/release-docs' CMakeLists.txt \
    || fail "missing release-docs CTest guard"
grep -q 'add_test(NAME docs/package-docs' CMakeLists.txt \
    || fail "missing package-docs CTest guard"
grep -q 'add_test(NAME docs/release-notes' CMakeLists.txt \
    || fail "missing release-notes CTest guard"
grep -q 'CONFLUX_PACKAGE_SMOKE_COMPONENTS' CMakeLists.txt \
    || fail "missing package smoke component cache variable"
grep -q 'add_test(NAME build/package-config-install-tree' CMakeLists.txt \
    || fail "missing installed-prefix package smoke CTest guard"
grep -q 'CONFLUX_RUN_INSTALL_TREE_SMOKE' CMakeLists.txt \
    || fail "missing opt-in install-tree smoke CTest option"
grep -q 'add_test(NAME build/install-tree-smoke' CMakeLists.txt \
    || fail "missing install-tree smoke CTest guard"
grep -q '"name": "release-header-artifacts"' CMakePresets.json \
    || fail "missing release-header-artifacts preset"
grep -q '"configurePreset": "release-clang-libcxx"' CMakePresets.json \
    || fail "missing release-clang-libcxx test preset"

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
grep -q 'import conflux.types;' cmake/package-smoke/CMakeLists.txt \
    || fail "module package smoke source must import an installed conflux module"
grep -q '#include <conflux/conflux.hpp>' cmake/package-smoke/CMakeLists.txt \
    || fail "header package smoke source must include the installed conflux umbrella header"
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
grep -q -- '--forbid-external-deps' scripts/run-package-config-smoke.sh \
    || fail "package smoke runner must expose negative external dependency assertions"
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
grep -q 'run-package-config-smoke.sh' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must consume the installed prefix"
grep -q -- '--enable-db-smoke' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must forward DB-enabled package smoke"
grep -q "core;json;file_io_sync" scripts/check-package-smoke-liburing-free.sh \
    || fail "liburing-free package smoke must request only liburing-free components"
grep -q "core;json;http;file_io_sync;runtime" scripts/check-package-smoke-runtime.sh \
    || fail "runtime package smoke must request runtime/http components"
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
