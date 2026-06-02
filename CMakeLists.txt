cmake_minimum_required(VERSION 3.30)

include(cmake/ConfluxImportStdGate.cmake)
conflux_pre_project_import_std_requested(_conflux_import_std_requested)
if(_conflux_import_std_requested)
    conflux_enable_import_std_experiment(WARN_UNSUPPORTED)
endif()
unset(_conflux_import_std_requested)

project(conflux VERSION 0.1.0 LANGUAGES CXX)

include(cmake/ConfluxOptions.cmake)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ConfluxPresets)
conflux_apply_preset()
include(ConfluxProviderSelection)

include(CompilerOptions)
include(ConfluxCompilerWorkarounds)
include(ConfluxModuleLibrary)
include(ConfluxSimd)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(ConfluxPython)
include(ConfluxInterfaceMode)
include(ConfluxUringProbes)
conflux_configure_interface_mode()

if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
    include(cmake/ConfluxHeaderInterface.cmake)
    return()
endif()

include(Dependencies)

include(ConfluxFeatures)
conflux_detect_aesni(CONFLUX_HAS_AESNI)
include(ConfluxCompilerProbes)
include(ConfluxOptionsTarget)

conflux_detect_stdsimd(_conflux_simd_backend)
conflux_add_stdsimd_targets("${_conflux_simd_backend}")

include(cmake/components/CoreTargets.cmake)
include(cmake/components/CryptoTargets.cmake)
include(cmake/components/JsonTargets.cmake)
conflux_attach_stdsimd_targets("${_conflux_simd_backend}")
include(ConfluxComponentValidation)
include(cmake/components/FileTargets.cmake)
include(cmake/components/TemplateTargets.cmake)
include(cmake/components/RuntimeTargets.cmake)
include(cmake/components/DnsTargets.cmake)
include(cmake/components/DbTargets.cmake)

include(cmake/components/HttpCoreTargets.cmake)
include(cmake/components/NetworkTargets.cmake)
include(cmake/components/HttpRouterTargets.cmake)
include(cmake/components/HttpSupportTargets.cmake)
include(cmake/components/HttpProtocolTargets.cmake)
include(cmake/components/HttpServerTargets.cmake)
include(cmake/components/HttpAppTargets.cmake)
include(cmake/components/HttpUmbrellaTargets.cmake)
include(cmake/components/AggregateTargets.cmake)

set(CONFLUX_EXAMPLES_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/examples")
if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE"
        AND NOT CONFLUX_IMPORT_STD_ENABLED
        AND EXISTS "${CONFLUX_GENERATED_EXAMPLES_DIR}")
    set(CONFLUX_EXAMPLES_SOURCE_ROOT "${CONFLUX_GENERATED_EXAMPLES_DIR}")
endif()

# ---------------------------------------------------------------------------
# Tests, benchmarks, and fuzzing
# ---------------------------------------------------------------------------

if(CONFLUX_BUILD_TESTS
        OR CONFLUX_BUILD_PACKAGE_TESTS
        OR (CONFLUX_BUILD_FUZZ AND CONFLUX_BUILD_FUZZ_SMOKE_TESTS))
    enable_testing()
endif()

if(CONFLUX_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
    if(TARGET conflux_uring)
        target_compile_definitions(conflux_uring PRIVATE CONFLUX_TESTING)
    endif()
    if(CONFLUX_BUILD_TESTS
            AND CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE"
            AND NOT CONFLUX_IMPORT_STD_ENABLED
            AND EXISTS "${CONFLUX_GENERATED_TESTS_DIR}/CMakeLists.txt")
        add_subdirectory("${CONFLUX_GENERATED_TESTS_DIR}" tests)
    else()
        add_subdirectory(tests)
    endif()
endif()

if(CONFLUX_BUILD_BENCHMARKS)
    if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE"
            AND NOT CONFLUX_IMPORT_STD_ENABLED
            AND EXISTS "${CONFLUX_GENERATED_BENCHMARKS_DIR}/CMakeLists.txt")
        add_subdirectory("${CONFLUX_GENERATED_BENCHMARKS_DIR}" benchmarks)
    else()
        add_subdirectory(benchmarks)
    endif()
endif()

if(CONFLUX_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Public package targets / install-export
# ---------------------------------------------------------------------------

include(cmake/ConfluxInstall.cmake)

include(cmake/ConfluxBuildChecks.cmake)
