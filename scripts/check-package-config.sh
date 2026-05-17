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
[[ -f cmake/package-smoke/CMakeLists.txt ]] || fail "missing package smoke project"
[[ -f scripts/run-package-config-smoke.sh ]] || fail "missing package smoke runner"
[[ -f scripts/run-install-tree-smoke.sh ]] || fail "missing install-tree smoke runner"
[[ -f scripts/check-cmake-source-files.py ]] || fail "missing CMake source-file guard"
[[ -f scripts/check-component-map.py ]] || fail "missing component-map guard"

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
grep -q 'NAMESPACE conflux::' CMakeLists.txt \
    || fail "export namespace must stay conflux::"
grep -q 'add_test(NAME build/cmake-source-files' CMakeLists.txt \
    || fail "missing CMake source-file CTest guard"
grep -q 'add_test(NAME build/component-map' CMakeLists.txt \
    || fail "missing component-map CTest guard"
grep -q 'add_test(NAME build/package-config' CMakeLists.txt \
    || fail "missing package-config CTest guard"
grep -q 'CONFLUX_PACKAGE_SMOKE_COMPONENTS' CMakeLists.txt \
    || fail "missing package smoke component cache variable"
grep -q 'add_test(NAME build/package-config-install-tree' CMakeLists.txt \
    || fail "missing installed-prefix package smoke CTest guard"
grep -q 'CONFLUX_RUN_INSTALL_TREE_SMOKE' CMakeLists.txt \
    || fail "missing opt-in install-tree smoke CTest option"
grep -q 'add_test(NAME build/install-tree-smoke' CMakeLists.txt \
    || fail "missing install-tree smoke CTest guard"

grep -q '@PACKAGE_INIT@' cmake/conflux-config.cmake.in \
    || fail "package config must use PACKAGE_INIT"
grep -q 'include(CMakeFindDependencyMacro)' cmake/conflux-config.cmake.in \
    || fail "package config must include CMakeFindDependencyMacro"
grep -q 'include("${CMAKE_CURRENT_LIST_DIR}/confluxTargets.cmake")' cmake/conflux-config.cmake.in \
    || fail "package config must include exported targets"
grep -q 'set(conflux_AVAILABLE_COMPONENTS' cmake/conflux-config.cmake.in \
    || fail "package config must expose available components"
grep -q 'foreach(_conflux_component IN LISTS conflux_FIND_COMPONENTS)' cmake/conflux-config.cmake.in \
    || fail "package config must validate requested components"
grep -q 'check_required_components(conflux)' cmake/conflux-config.cmake.in \
    || fail "package config must call check_required_components(conflux)"
grep -q 'conflux::conflux' cmake/conflux-config.cmake.in \
    || fail "package config must provide the canonical umbrella alias when available"

grep -q 'find_package(conflux REQUIRED COMPONENTS' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke project must consume find_package(conflux COMPONENTS ...)"
grep -q 'add_executable(conflux_package_smoke package_smoke.cxx)' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke project must compile a downstream executable"
grep -q 'target_link_libraries(conflux_package_smoke PRIVATE' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke project must link installed namespaced targets"
grep -q 'add_test(NAME package-smoke/run' cmake/package-smoke/CMakeLists.txt \
    || fail "package smoke project must run the downstream executable"
grep -q 'import conflux.types;' cmake/package-smoke/package_smoke.cxx \
    || fail "package smoke source must import an installed conflux module"
grep -q 'cmake --build "\$build_dir"' scripts/run-package-config-smoke.sh \
    || fail "package smoke runner must build the downstream project"
grep -q 'ctest --test-dir "\$build_dir" --output-on-failure' scripts/run-package-config-smoke.sh \
    || fail "package smoke runner must run downstream CTest"
grep -q 'cmake --build "\$build_dir" --target install' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must build and install conflux"
grep -q 'run-package-config-smoke.sh' scripts/run-install-tree-smoke.sh \
    || fail "install-tree smoke runner must consume the installed prefix"

printf 'check-package-config: ok\n'
