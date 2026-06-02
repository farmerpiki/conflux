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
[[ -f cmake/ConfluxComponentValidation.cmake ]] || fail "missing component validation CMake module"
[[ -f cmake/ConfluxCompilerProbes.cmake ]] || fail "missing compiler probes CMake module"
[[ -f cmake/ConfluxCompilerWorkarounds.cmake ]] || fail "missing compiler workaround CMake module"
[[ -f cmake/ConfluxModuleLibrary.cmake ]] || fail "missing module-library helper CMake module"
[[ -f cmake/ConfluxOptionsTarget.cmake ]] || fail "missing options target CMake module"
[[ -f cmake/ConfluxProviderSelection.cmake ]] || fail "missing provider selection CMake module"
[[ -f cmake/ConfluxPython.cmake ]] || fail "missing Python configuration CMake module"
[[ -f cmake/ConfluxUringProbes.cmake ]] || fail "missing io_uring probe CMake module"
[[ -f cmake/package-smoke/CMakeLists.txt ]] || fail "missing package smoke project"
[[ -f scripts/run-package-config-smoke.sh ]] || fail "missing package smoke runner"
[[ -f scripts/run-install-tree-smoke.sh ]] || fail "missing install-tree smoke runner"
[[ -f scripts/check-header-first-contact-smoke.sh ]] || fail "missing first-contact header smoke runner"
[[ -f scripts/check-header-component-smoke.sh ]] || fail "missing full header component smoke runner"
[[ -f scripts/check-package-smoke-liburing-free.sh ]] || fail "missing liburing-free package smoke lane"
[[ -f scripts/check-package-smoke-core-isolated.sh ]] || fail "missing core-isolated package smoke lane"
[[ -f scripts/check-package-smoke-runtime.sh ]] || fail "missing runtime package smoke lane"
[[ -f scripts/check-package-smoke-db.sh ]] || fail "missing DB package smoke lane"
[[ -f scripts/check-provider-policy-matrix.sh ]] || fail "missing provider-policy matrix guard"
[[ -f scripts/check-cmake-source-files.py ]] || fail "missing CMake source-file guard"
[[ -f scripts/check-component-map.py ]] || fail "missing component-map guard"
[[ -f scripts/cmake-preset-build-dir.py ]] || fail "missing CMake preset build-dir helper"
[[ -f scripts/check-cmake-preset-build-dir.py ]] || fail "missing CMake preset build-dir helper guard"
[[ -f scripts/check-package-config-structure.py ]] || fail "missing package-config structure guard"
[[ -f scripts/check-http-facade-snapshot.py ]] || fail "missing HTTP facade snapshot guard"
[[ -f scripts/check-planning-state.py ]] || fail "missing planning-state guard"
[[ -f scripts/check-release-docs.py ]] || fail "missing release-docs guard"
[[ -f scripts/check-package-docs.py ]] || fail "missing package-docs guard"
[[ -f scripts/check-release-notes.py ]] || fail "missing release-notes guard"
[[ -f tests/BuildAndDocsChecks.cmake ]] || fail "missing build/docs CTest registration fragment"
[[ -f scripts/stage-release-artifacts.sh ]] || fail "missing release artifact staging script"
[[ -f scripts/check-release-artifact.py ]] || fail "missing release artifact guard"

python3 scripts/check-package-config-structure.py . \
    || fail "package-config structure guard failed"
grep -q 'check_metrics_status_is_graph_gated' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep metrics status scoped to active HTTP graphs"
grep -q 'check_duplicate_ctest_names' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject duplicate CTest names"
grep -q 'for path in cmake_test_cmake_paths()' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must scan every CTest registration fragment"
grep -q 'def check_marker_order' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must centralize marker-order checks"
grep -q 'check_cmake_preset_names_unique' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject duplicate CMake preset names"
grep -q 'has missing or non-string name' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject malformed CMake preset names"
grep -q 'check_cmake_preset_references' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate CMake preset references"
grep -q 'references missing configurePreset' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale CMake preset references"
grep -q 'must match configurePreset' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep build preset names aligned with configure presets"
grep -q 'check_test_preset_filters' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate exact test preset filters"
grep -q 'filters unknown CTest name' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale exact test preset name filters"
grep -q 'filters unknown CTest label' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale test preset label filters"
grep -q 'check_matrix_script_presets' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate matrix script preset lists"
grep -q 'matrix presets missing from CMakePresets.json' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale matrix preset names"
grep -q 'check_build_all_presets' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate build-all preset list"
grep -q 'build-all.sh: presets missing from CMakePresets.json' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale build-all preset names"
grep -q 'check_script_default_presets' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate script default preset lists"
grep -q 'default presets missing from CMakePresets.json' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale script default preset names"
grep -q 'BENCH_PRESETS' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate benchmark recorder default presets"
grep -q 'check_script_default_benchmark_targets' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate script default benchmark target lists"
grep -q 'default benchmark targets are not declared by CMake' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale script default benchmark targets"
grep -q 'check_json_perf_benchmark_maps' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate JSON perf benchmark maps"
grep -q 'JSON perf default benches must match JSON perf default targets' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject mismatched JSON perf defaults"
grep -q 'check_no_explicit_build_parallelism' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject explicit build parallelism"
grep -q 'explicit build parallelism is not allowed' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must explain explicit build parallelism failures"
grep -q 'check_provider_policy_scenarios_are_isolated' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify provider-policy scenario isolation"
grep -q 'provider-policy isolation flags' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject provider-policy scenarios that pull tests/examples/benchmarks"
grep -q 'check_run_build_artifact_root_examples_are_declared' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate run-build-artifact root example allowlist"
grep -q 'undeclared example targets' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale run-build-artifact example entries"
grep -q 'check_compile_time_bench_defaults' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate compile-time benchmark defaults"
grep -q 'compile-time benchmark default targets are not declared by CMake' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale compile-time benchmark targets"
grep -q 'compile-time benchmark incremental sources are missing' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale compile-time benchmark sources"
python3 scripts/check-empty-catch-rationale.py . \
    || fail "empty-catch rationale guard failed"

grep -Eq '^project\(conflux VERSION [0-9]+\.[0-9]+\.[0-9]+ LANGUAGES CXX\)' CMakeLists.txt \
    || fail "project() must declare the package version"
grep -q 'check_cmake_extraction_contracts' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify CMake extraction contracts"
grep -q 'compiler workaround must keep its GCC ICE motivation' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep compiler workaround motivations"

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
grep -q 'check_package_metadata_generator_contract' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify package metadata generator validation"
grep -q 'package metadata generator must reject component partition drift' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep package metadata partition validation"
grep -q 'NAMESPACE conflux::' cmake/ConfluxInstall.cmake \
    || fail "export namespace must stay conflux::"
grep -q 'include("${CMAKE_CURRENT_LIST_DIR}/BuildAndDocsChecks.cmake")' tests/CMakeLists.txt \
    || fail "tests CMake must include the build/docs CTest registration fragment"
grep -q 'add_library(${CF_TARGET} EXCLUDE_FROM_ALL OBJECT ${CF_SOURCE})' tests/CMakeLists.txt \
    || fail "compile-fail checks must stay compile-time OBJECT target checks"
grep -q 'scripts/check-compile-fail-target.sh' tests/CMakeLists.txt \
    || fail "compile-fail checks must use the compile-fail target runner"
grep -q 'add_test(NAME build/cmake-source-files' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "missing CMake source-file CTest guard"
grep -q 'ROOT / "tests"' scripts/check-cmake-source-files.py \
    || fail "CMake source-file guard must scan test CMake fragments"
grep -q 'ROOT / "tests" / "CMakeLists.txt"' scripts/check-cmake-source-files.py \
    || fail "CMake source-file guard must scan tests CMakeLists"
grep -q 'ROOT / "cmake" / "package-smoke" / "CMakeLists.txt"' scripts/check-cmake-source-files.py \
    || fail "CMake source-file guard must scan package-smoke CMakeLists"
grep -q 'ROOT / "benchmarks" / "CMakeLists.txt"' scripts/check-cmake-source-files.py \
    || fail "CMake source-file guard must scan benchmarks CMakeLists"
grep -q 'ROOT / "examples" / "CMakeLists.txt"' scripts/check-cmake-source-files.py \
    || fail "CMake source-file guard must scan examples CMakeLists"
grep -q 'ROOT / "fuzz" / "CMakeLists.txt"' scripts/check-cmake-source-files.py \
    || fail "CMake source-file guard must scan fuzz CMakeLists"
grep -q 'EXTENSION_PATTERN = "|".join' scripts/check-cmake-source-files.py \
    || fail "CMake source-file guard must derive its regex from SOURCE_EXTENSIONS"
grep -q 'add_test(NAME build/component-map' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "missing component-map CTest guard"
grep -q 'declared as both public and support' scripts/check-component-map.py \
    || fail "component-map guard must reject public/support component ownership overlap"
grep -q 'public component `.*` uses an unsafe export name' scripts/check-component-map.py \
    || fail "component-map guard must reject unsafe public component export names"
grep -q 'support component `.*` uses an unsafe export name' scripts/check-component-map.py \
    || fail "component-map guard must reject unsafe support component export names"
grep -q 'add_test(NAME build/http-facade-snapshot' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "missing HTTP facade snapshot CTest guard"
grep -q 'add_test(NAME build/package-config' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "missing package-config CTest guard"
grep -q 'add_test(NAME build/module-fragility-regression' tests/BuildAndDocsChecks.cmake \
    || fail "module-fragility CTest guard must live in build/docs fragment"
grep -q 'add_test(NAME build/optimized-presets' tests/BuildAndDocsChecks.cmake \
    || fail "optimized-presets CTest guard must live in build/docs fragment"
grep -q 'add_test(NAME build/cmake-preset-build-dir' tests/BuildAndDocsChecks.cmake \
    || fail "CMake preset build-dir helper CTest guard must live in build/docs fragment"
grep -q 'duplicate configure preset' scripts/cmake-preset-build-dir.py \
    || fail "CMake preset build-dir helper must reject duplicate configure preset names"
grep -q 'cyclic preset include involving' scripts/cmake-preset-build-dir.py \
    || fail "CMake preset build-dir helper must reject cyclic preset includes"
grep -q 'cyclic preset inheritance involving' scripts/cmake-preset-build-dir.py \
    || fail "CMake preset build-dir helper must reject cyclic preset inheritance"
grep -q 'cyclic preset includes must be rejected' scripts/check-cmake-preset-build-dir.py \
    || fail "CMake preset build-dir helper guard must cover cyclic include rejection"
grep -q 'cyclic preset inheritance must be rejected' scripts/check-cmake-preset-build-dir.py \
    || fail "CMake preset build-dir helper guard must cover cyclic inheritance rejection"
grep -q 'add_test(NAME build/header-first-contact-smoke' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "missing default first-contact header smoke CTest guard"
grep -q 'CONFLUX_RUN_HEADER_COMPONENT_SMOKE' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "full header component smoke must be opt-in"
grep -q 'add_test(NAME build/header-component-smoke' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "missing full header component smoke CTest guard"
grep -q 'add_test(NAME docs/planning-state' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "missing planning-state CTest guard"
grep -q 'add_test(NAME docs/release-docs' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "missing release-docs CTest guard"
grep -q 'add_test(NAME docs/package-docs' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "missing package-docs CTest guard"
grep -q 'add_test(NAME docs/release-notes' tests/CMakeLists.txt tests/BuildAndDocsChecks.cmake \
    || fail "missing release-notes CTest guard"
grep -q 'CONFLUX_PACKAGE_SMOKE_COMPONENTS' cmake/ConfluxOptions.cmake \
    || fail "missing package smoke component cache variable"
grep -q 'add_test(NAME build/package-config-install-tree' cmake/ConfluxOptions.cmake \
    || fail "missing installed-prefix package smoke CTest guard"
grep -q 'CONFLUX_BUILD_PACKAGE_TESTS' cmake/ConfluxOptions.cmake \
    || fail "missing package-only CTest option"
grep -q 'CONFLUX_HEADER_FAST_COMPILE' cmake/ConfluxOptions.cmake \
    || fail "missing header fast-compile option"
grep -q 'CONFLUX_HEADER_LINK_EXAMPLES' cmake/ConfluxOptions.cmake \
    || fail "missing opt-in linked header examples option"
grep -q 'CONFLUX_HEADER_LINK_SMOKE' cmake/ConfluxOptions.cmake \
    || fail "missing opt-in linked header smoke option"
grep -q 'CONFLUX_RUN_HEADER_COMPONENT_SMOKE' cmake/ConfluxOptions.cmake \
    || fail "missing opt-in full header component smoke option"
grep -q 'conflux_header_smoke_api_surface_curated' scripts/check-header-first-contact-smoke.sh \
    || fail "first-contact header smoke must build only the curated API surface target"
grep -q 'CONFLUX_HEADER_COMPONENT_SMOKE_BUILD_ROOT' scripts/check-header-component-smoke.sh \
    || fail "full header component smoke must remain separately configurable"
grep -q 'CXX_SCAN_FOR_MODULES OFF' cmake/ConfluxInterfaceMode.cmake \
    || fail "header generated targets must disable module scanning"
grep -q 'CONFLUX_HEADER_FAST_COMPILE' cmake/ConfluxInterfaceMode.cmake \
    || fail "header generated targets must honor fast-compile option"
grep -q 'CONFLUX_HEADER_LINK_EXAMPLES' cmake/ConfluxInterfaceMode.cmake \
    || fail "header examples must keep implementation linking opt-in"
grep -q 'CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE"' cmake/ConfluxOptions.cmake \
    || fail "API surface definitions must handle header mode separately"
grep -q 'CONFLUX_WANT_HTTP_POLICY' cmake/ConfluxOptions.cmake \
    || fail "header API surface macros must derive from resolved component flags"
grep -q 'check_header_http_impls_do_not_pull_json' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must protect header HTTP/JSON implementation isolation"
grep -q 'header example registration must parse explicit implementation deps' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must require explicit header example implementation dependency parsing"
grep -q 'dual header example must declare HTTP client implementation deps explicitly' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep the dual header example dependency declaration explicit"
grep -q 'linked header examples must declare implementation deps explicitly' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject source-id-based header implementation inference"
grep -q 'router_impl owns serve_static' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must motivate the header HTTP static implementation fallback"
grep -q 'check_header_impl_lists_have_no_duplicates' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject duplicate header implementation dependency entries"
grep -q 'check_header_source_ids_exist' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject stale header bridge source ids"
grep -q 'header bridge source ids are missing backing .cxx files' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must explain stale header bridge source ids"
grep -q 'check_header_support_components_are_limited' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must limit manually managed header support components"
grep -q 'generated-header support export' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must make generated-header support export exceptions explicit"
grep -q 'support registry export' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject unknown header support export names"
grep -q 'check_header_public_components_use_registry_exports' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep header package components registry-derived"
grep -q 'is not a public registry export' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject header package components outside the public registry"
grep -q 'check_package_smoke_wrapper_default_components' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must validate wrapper default package smoke components"
grep -q 'wrapper default package smoke components must be public registry exports' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep wrapper smoke defaults on public components"
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
grep -q 'add_test(NAME build/install-tree-smoke' cmake/ConfluxOptions.cmake \
	|| fail "missing install-tree smoke CTest guard"
grep -q 'set(CMAKE_CXX_SCAN_FOR_MODULES OFF)' cmake/ConfluxOptions.cmake \
	|| fail "HEADER_INTERFACE must disable CMake module scanning"
grep -q '"name": "release-header-artifacts"' CMakePresets.json \
    || fail "missing release-header-artifacts preset"
grep -A14 '"name": "release-header-artifacts"' CMakePresets.json | grep -q '"CONFLUX_FEATURE_SET": "release-json"' \
    || fail "release-header-artifacts must pin release-json feature set"
grep -q '"configurePreset": "release-clang-libcxx"' CMakePresets.json \
    || fail "missing release-clang-libcxx test preset"
grep -q '@PACKAGE_INIT@' cmake/conflux-config.cmake.in \
    || fail "package config must use PACKAGE_INIT"
grep -q 'include(CMakeFindDependencyMacro)' cmake/conflux-config.cmake.in \
    || fail "package config must include CMakeFindDependencyMacro"
grep -q 'check_package_config_uses_generated_component_metadata' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify generated package component metadata use"
grep -q 'hand-written component dependency table' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject hand-written package component dependency tables"
if grep -q 'target_link_libraries *( *conflux_headers .*PkgConfig::LIBURING' cmake/ConfluxInterfaceMode.cmake; then
    fail "header support target must not leak liburing into every header package component"
fi
if grep -q 'target_link_libraries *( *conflux_headers .*PkgConfig::XXHASH' cmake/ConfluxInterfaceMode.cmake; then
    fail "header support target must not leak xxhash into every header package component"
fi
grep -q 'set(CONFLUX_RUNTIME_REQUIRES_LIBURING' cmake/conflux-config.cmake.in \
    || fail "package config must expose runtime liburing status"
grep -q 'foreach(_conflux_component IN LISTS conflux_FIND_COMPONENTS)' cmake/conflux-config.cmake.in \
    || fail "package config must validate requested components"
grep -q 'check_required_components(conflux)' cmake/conflux-config.cmake.in \
    || fail "package config must call check_required_components(conflux)"
grep -q 'conflux::conflux' cmake/conflux-config.cmake.in \
    || fail "package config must provide the canonical umbrella alias when available"
grep -q 'GIT_REPOSITORY https://github.com/nst/JSONTestSuite.git' cmake/Dependencies.cmake \
    || fail "JSONTestSuite fetch must name the upstream repository explicitly"
if grep -A6 'GIT_REPOSITORY https://github.com/nst/JSONTestSuite.git' cmake/Dependencies.cmake | grep -Eq 'GIT_TAG[[:space:]]+(master|main)$'; then
    fail "JSONTestSuite fetch must not use a floating branch"
fi
grep -A6 'GIT_REPOSITORY https://github.com/nst/JSONTestSuite.git' cmake/Dependencies.cmake | grep -Eq 'GIT_TAG[[:space:]]+[0-9a-f]{40}$' \
    || fail "JSONTestSuite fetch must pin a full commit SHA"
grep -A6 'GIT_REPOSITORY https://github.com/nst/JSONTestSuite.git' cmake/Dependencies.cmake | grep -q 'GIT_SHALLOW    FALSE' \
    || fail "JSONTestSuite full SHA fetch must not be shallow"

grep -q 'check_package_smoke_project_contract' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify the package smoke project contract"
grep -q 'found unrequested visible target' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep package smoke target-visibility rejection"
grep -q 'check_package_smoke_runner_contract' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify the package smoke runner contract"
grep -q 'package smoke runner must reject support components' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep package smoke runner support-component rejection"
grep -q 'check_preset_build_dir_usage_contracts' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify preset-derived build directory usage"
grep -q 'matrix scripts must not reconstruct preset build dirs' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject hardcoded matrix build directories"
grep -q 'check_install_tree_smoke_runner_contract' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify the install-tree smoke runner contract"
grep -q 'install-tree smoke runner must reject support components' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep install-tree smoke runner support-component rejection"
grep -q 'check_install_tree_ctest_helpers' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify install-tree smoke CTest helper wiring"
grep -q 'mixed-module-header-smoke>' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject empty-argument generator expressions in install-tree smoke CTests"
grep -q 'check_package_smoke_wrapper_contracts' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify package smoke wrapper contracts"
grep -q 'core-isolated package smoke must force the external JSON hash provider' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep core-isolated provider-noise coverage"
grep -q 'external tokens missing from core-isolated forbidden list' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify full core-isolated external-token coverage"
grep -q 'def cmake_function_body' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must centralize CMake function-body extraction"
grep -q 'def shell_semicolon_list_var' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must parse shell semicolon-list policies"
grep -q 'def append_set_delta_errors' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must centralize set-delta error reporting"
grep -q 'contains unknown external tokens' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject unknown tokens in package smoke external-dependency policies"
grep -q 'contains unknown package components' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject unknown package components in package smoke component policies"
grep -q 'check_core_isolated_forbidden_components' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must compare core-isolated component isolation with default core smoke policy"
grep -q 'check_package_smoke_policy_cases_use_variables' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must require package smoke policy cases to use named variables"
grep -q 'policy variables are not used' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject unused package smoke policy variables"
grep -q 'Path("cmake/package-smoke/CMakeLists.txt")' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must scan duplicate link edges outside root CMake"
grep -q 'requests non-public package smoke components' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject non-public install-smoke preset components"
grep -q 'check_release_artifact_staging_contract' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify release artifact staging"
grep -q 'release artifact guard must validate bridge python metadata' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep release artifact bridge metadata validation"

printf 'check-package-config: ok\n'
