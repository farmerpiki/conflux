#!/usr/bin/env bash
set -euo pipefail

root="${1:-$(pwd)}"
cd "$root"

fail() {
    printf 'check-package-config: %s\n' "$*" >&2
    exit 1
}

require_structure_guard_markers() {
    local marker diagnostic
    while IFS='|' read -r marker diagnostic; do
        [[ -n "$marker" ]] || continue
        grep -q -- "$marker" scripts/check-package-config-structure.py \
            || fail "$diagnostic"
    done
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
require_structure_guard_markers <<'EOF'
check_metrics_status_is_graph_gated|package-config structure guard must keep metrics status scoped to active HTTP graphs
check_duplicate_ctest_names|package-config structure guard must reject duplicate CTest names
for path in cmake_test_cmake_paths()|package-config structure guard must scan every CTest registration fragment
def check_marker_order|package-config structure guard must centralize marker-order checks
check_cmake_preset_names_unique|package-config structure guard must reject duplicate CMake preset names
has missing or non-string name|package-config structure guard must reject malformed CMake preset names
check_cmake_preset_references|package-config structure guard must validate CMake preset references
references missing configurePreset|package-config structure guard must reject stale CMake preset references
must match configurePreset|package-config structure guard must keep build preset names aligned with configure presets
check_test_preset_filters|package-config structure guard must validate exact test preset filters
filters unknown CTest name|package-config structure guard must reject stale exact test preset name filters
filters unknown CTest label|package-config structure guard must reject stale test preset label filters
check_matrix_script_presets|package-config structure guard must validate matrix script preset lists
matrix presets missing from CMakePresets.json|package-config structure guard must reject stale matrix preset names
check_build_all_presets|package-config structure guard must validate build-all preset list
build-all.sh: presets missing from CMakePresets.json|package-config structure guard must reject stale build-all preset names
check_script_default_presets|package-config structure guard must validate script default preset lists
default presets missing from CMakePresets.json|package-config structure guard must reject stale script default preset names
BENCH_PRESETS|package-config structure guard must validate benchmark recorder default presets
check_script_default_benchmark_targets|package-config structure guard must validate script default benchmark target lists
default benchmark targets are not declared by CMake|package-config structure guard must reject stale script default benchmark targets
check_json_perf_benchmark_maps|package-config structure guard must validate JSON perf benchmark maps
JSON perf default benches must match JSON perf default targets|package-config structure guard must reject mismatched JSON perf defaults
check_no_explicit_build_parallelism|package-config structure guard must reject explicit build parallelism
explicit build parallelism is not allowed|package-config structure guard must explain explicit build parallelism failures
check_provider_policy_scenarios_are_isolated|package-config structure guard must verify provider-policy scenario isolation
provider-policy isolation flags|package-config structure guard must reject provider-policy scenarios that pull tests/examples/benchmarks
check_run_build_artifact_root_examples_are_declared|package-config structure guard must validate run-build-artifact root example allowlist
undeclared example targets|package-config structure guard must reject stale run-build-artifact example entries
check_compile_time_bench_defaults|package-config structure guard must validate compile-time benchmark defaults
compile-time benchmark default targets are not declared by CMake|package-config structure guard must reject stale compile-time benchmark targets
compile-time benchmark incremental sources are missing|package-config structure guard must reject stale compile-time benchmark sources
EOF
python3 scripts/check-empty-catch-rationale.py . \
    || fail "empty-catch rationale guard failed"

grep -Eq '^project\(conflux VERSION [0-9]+\.[0-9]+\.[0-9]+ LANGUAGES CXX\)' CMakeLists.txt \
    || fail "project() must declare the package version"
grep -q 'check_cmake_extraction_contracts' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify CMake extraction contracts"
grep -q 'compiler workaround must keep its GCC ICE motivation' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep compiler workaround motivations"

grep -q 'check_install_and_dependency_contracts' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify install and dependency contracts"
grep -q 'JSONTestSuite fetch must pin a full commit SHA' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep JSONTestSuite pin validation"
grep -q 'check_package_metadata_generator_contract' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify package metadata generator validation"
grep -q 'package metadata generator must reject component partition drift' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep package metadata partition validation"
grep -q 'check_build_docs_guard_contracts' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify build/docs guard contracts"
grep -q 'compile-fail checks must stay compile-time OBJECT target checks' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep compile-fail CTest contract"
grep -q 'check_header_interface_contracts' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify header interface contracts"
grep -q 'release-header-artifacts must pin release-json feature set' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must keep release-header-artifacts preset checks"
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
grep -q 'check_package_config_uses_generated_component_metadata' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must verify generated package component metadata use"
grep -q 'hand-written component dependency table' scripts/check-package-config-structure.py \
    || fail "package-config structure guard must reject hand-written package component dependency tables"
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
