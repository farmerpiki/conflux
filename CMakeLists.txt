cmake_minimum_required(VERSION 3.30)

if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3.0")
    # Covers 4.3.0 up to future stable releases.
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.3")
    # Covers 4.0.3 up to 4.2.x.
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.0")
    # Covers 4.0.0, 4.0.1, and 4.0.2.
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "a9e1cf81-9932-4810-974b-6eccaf14e457")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31.8")
    # Covers late 3.31 patches backported with 4.1 features.
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.30.0")
    # Covers initial experimental introduction up to 3.31.7.
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
else()
    message(WARNING "Your CMake version does not support experimental 'import std'.")
endif()

project(conflux VERSION 0.1.0 LANGUAGES CXX)

option(CONFLUX_JSON_REFLECT   "Enable P2996 reflection codec (GCC 16+ only)" OFF)
set(CONFLUX_INTERFACE_MODE "MODULE_INTERFACE" CACHE STRING
    "Public consumer interface: MODULE_INTERFACE or HEADER_INTERFACE")
set_property(CACHE CONFLUX_INTERFACE_MODE PROPERTY STRINGS MODULE_INTERFACE HEADER_INTERFACE)
option(CONFLUX_USE_MOCK_LIBURING
    "Use generated compile-only mock liburing when system liburing is unavailable; not a runtime backend" OFF)
option(CONFLUX_HEADER_INTERFACE_WITH_SOURCES
    "Attach generated non-module implementation sources to conflux::headers; experimental" OFF)

if(NOT CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE"
        AND NOT CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
    message(FATAL_ERROR
        "CONFLUX_INTERFACE_MODE must be MODULE_INTERFACE or HEADER_INTERFACE; got '${CONFLUX_INTERFACE_MODE}'")
endif()

if(CONFLUX_JSON_REFLECT)
    set(CMAKE_CXX_STANDARD 26)
else()
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    if(NOT ${CMAKE_CXX_STANDARD} IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
        set(_conflux_import_std_levels "${CMAKE_CXX_COMPILER_IMPORT_STD}")
        if(_conflux_import_std_levels STREQUAL "")
            set(_conflux_import_std_levels "<none>")
        endif()
        message(FATAL_ERROR
            "conflux requires CMake-discoverable C++${CMAKE_CXX_STANDARD} import std support in MODULE_INTERFACE mode. "
            "Supported import std standard levels for this toolchain: ${_conflux_import_std_levels}. "
            "Use CONFLUX_INTERFACE_MODE=HEADER_INTERFACE to generate the header consumer surface.")
    endif()
    set(CMAKE_CXX_MODULE_STD ON)
else()
    set(CMAKE_CXX_MODULE_STD OFF)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(CONFLUX_ENABLE_ASAN    "Build with AddressSanitizer"                  OFF)
option(CONFLUX_ENABLE_UBSAN   "Build with UBSanitizer"                       OFF)
option(CONFLUX_ENABLE_TSAN    "Build with ThreadSanitizer"                   OFF)
option(CONFLUX_ENABLE_LTO     "Enable link-time optimisation"                OFF)
set(CONFLUX_LTO_MODE "AUTO" CACHE STRING "LTO mode when CONFLUX_ENABLE_LTO=ON: AUTO, THIN, or FULL")
set_property(CACHE CONFLUX_LTO_MODE PROPERTY STRINGS AUTO THIN FULL)
if(CONFLUX_JSON_REFLECT
        AND CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE"
        AND NOT 26 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    set(_conflux_import_std_levels "${CMAKE_CXX_COMPILER_IMPORT_STD}")
    if(_conflux_import_std_levels STREQUAL "")
        set(_conflux_import_std_levels "<none>")
    endif()
    message(FATAL_ERROR
        "conflux: CONFLUX_JSON_REFLECT requires C++26 import std support in MODULE_INTERFACE mode. "
        "Supported import std standard levels for this toolchain: ${_conflux_import_std_levels}. "
        "Use CONFLUX_INTERFACE_MODE=HEADER_INTERFACE to avoid import std.")
endif()
option(CONFLUX_PGO_GENERATE  "Instrument build for PGO profile collection"  OFF)
set(CONFLUX_PGO_PROFILE_DIR  "" CACHE STRING "Profile data path for PGO use phase")
option(CONFLUX_BUILD_TESTS    "Build test targets"                           ON)
option(CONFLUX_FETCH_TEST_DEPS "Allow FetchContent downloads for test-only dependencies" ON)
set(CONFLUX_TEST_CATCH2_PROVIDER "FETCH" CACHE STRING
    "Catch2 provider for tests: FETCH builds Catch2 with the active stdlib, SYSTEM uses find_package, AUTO tries SYSTEM then FETCH")
set_property(CACHE CONFLUX_TEST_CATCH2_PROVIDER PROPERTY STRINGS FETCH SYSTEM AUTO)
set(CONFLUX_CATCH2_SOURCE_DIR "" CACHE PATH "Path to a local Catch2 source tree for CONFLUX_TEST_CATCH2_PROVIDER=FETCH")
option(CONFLUX_ENABLE_JSON_TESTSUITE "Build nst/JSONTestSuite conformance gate when JSONTestSuite is available" OFF)
set(JSONTESTSUITE_DIR "" CACHE PATH "Path to nst/JSONTestSuite/test_parsing, or repository root containing test_parsing")
option(CONFLUX_BUILD_BENCHMARKS "Build benchmark targets"                    OFF)
option(CONFLUX_BUILD_EXAMPLES "Build example targets"                        ON)
option(CONFLUX_ALLOW_SANITIZED_BENCHMARKS
    "Allow benchmark targets in sanitizer builds for local instrumentation debugging only" OFF)
option(CONFLUX_BUILD_FUZZ     "Build libFuzzer harness targets"              OFF)
option(CONFLUX_BUILD_FUZZ_SMOKE_TESTS
    "Register bounded CTest smoke tests for libFuzzer harnesses" ON)
option(CONFLUX_ENABLE_TLS     "Build TLS support (requires OpenSSL)"         ON)
option(CONFLUX_ENABLE_ZLIB    "Build classic zlib gzip backend"              ON)
option(CONFLUX_ENABLE_LIBDEFLATE "Build libdeflate gzip backend"             ON)
option(CONFLUX_ENABLE_ZLIB_NG "Build zlib-ng gzip backend"                   ON)
option(CONFLUX_ENABLE_ISAL    "Build ISA-L gzip backend"                     ON)
option(CONFLUX_ENABLE_BROTLI  "Build Brotli compression support"             ON)
option(CONFLUX_ENABLE_ZSTD    "Build Zstandard compression support"          ON)
option(CONFLUX_ENABLE_METRICS "Build Prometheus-compatible metrics support"  ON)
option(CONFLUX_ENABLE_EXPERIMENTAL "Enable experimental runtime features and their tests/benchmarks" OFF)
option(CONFLUX_ENABLE_RECV_BUNDLE "Allow io_uring multishot recv bundling when configured and supported by the kernel" ON)
option(CONFLUX_ENABLE_RECV_INCREMENTAL_BUF "Allow experimental incremental provided-buffer rings when configured and supported by the kernel" ON)
option(CONFLUX_ENABLE_SEND_ZC "Allow experimental io_uring SEND_ZC when configured and supported by the kernel" ON)
option(CONFLUX_ENABLE_RING_GROWTH "Allow experimental io_uring CQ ring growth after overflow" ON)
option(CONFLUX_ENABLE_IOPOLL_STORAGE_TEST "Allow experimental IOPOLL/O_DIRECT storage-ring test coverage" ON)
option(CONFLUX_ENABLE_HTTP_TRACE "Enable debug-only HTTP/server tracing"     OFF)
option(CONFLUX_ENABLE_HTTP2   "Build HTTP/2 support (requires nghttp2+TLS)"  ON)
option(CONFLUX_ENABLE_HTTP3   "Build HTTP/3 support (requires ngtcp2+nghttp3+OpenSSL 3.5+TLS)" ON)
option(CONFLUX_ENABLE_DB      "Build PostgreSQL client support (requires libpq)" ON)
option(CONFLUX_WORK_CARRIER_MODEL_A "Enable experimental work carrier Model A prototype surface" ON)
option(CONFLUX_WORK_CARRIER_MODEL_B "Enable experimental work carrier Model B prototype surface" OFF)
set(CONFLUX_PASSWORD_HASH_ARGON2_PROVIDER "AUTO" CACHE STRING
    "Argon2 backend for password hashing: AUTO uses system libargon2 when found, otherwise runtime dlopen; SYSTEM requires libargon2; RUNTIME uses dlopen only; OFF disables Argon2")
set_property(CACHE CONFLUX_PASSWORD_HASH_ARGON2_PROVIDER PROPERTY STRINGS AUTO SYSTEM RUNTIME OFF)
option(CONFLUX_WORK_CORO_FRAME_POOL "Enable pooled coroutine frame allocation for work Task/EagerChain frames" OFF)
option(CONFLUX_WORK_ALLOC_STATS "Enable relaxed allocation counters for work.root task/control-block diagnostics" OFF)
option(CONFLUX_WORK_QUEUE_STATS "Enable relaxed WorkPool queue/wake contention counters for profiling" OFF)
option(CONFLUX_GCC_EAGER_MODULE_IMPORTS "Force GCC module imports to eager-load with -fno-module-lazy for local compiler diagnostics" OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
        AND CONFLUX_GCC_EAGER_MODULE_IMPORTS
        AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15")
    if(NOT CMAKE_CXX_FLAGS MATCHES "(^| )-fno-module-lazy( |$)")
        string(APPEND CMAKE_CXX_FLAGS " -fno-module-lazy")
    endif()
endif()
option(CONFLUX_GCC_SUGGEST_ATTRIBUTES "Enable noisy GCC -Wsuggest-attribute=pure/const optimization hints" OFF)
set(CONFLUX_USE_STDSIMD "AUTO" CACHE STRING
    "Use standard/experimental SIMD for library scan paths (AUTO|STD26|STDX|ON|OFF)")
set_property(CACHE CONFLUX_USE_STDSIMD PROPERTY STRINGS AUTO STD26 STDX ON OFF)
if(DEFINED CONFLUX_JSON_USE_STDSIMD)
    set(CONFLUX_USE_STDSIMD "${CONFLUX_JSON_USE_STDSIMD}" CACHE STRING
        "Use standard/experimental SIMD for library scan paths (AUTO|STD26|STDX|ON|OFF)" FORCE)
    message(DEPRECATION
        "conflux: CONFLUX_JSON_USE_STDSIMD is deprecated; use CONFLUX_USE_STDSIMD")
endif()

set(CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF OFF)
set(CONFLUX_EXPERIMENTAL_SEND_ZC OFF)
set(CONFLUX_EXPERIMENTAL_RING_GROWTH OFF)
set(CONFLUX_EXPERIMENTAL_IOPOLL_STORAGE_TEST OFF)
if(CONFLUX_ENABLE_EXPERIMENTAL)
    set(CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF ${CONFLUX_ENABLE_RECV_INCREMENTAL_BUF})
    set(CONFLUX_EXPERIMENTAL_SEND_ZC ${CONFLUX_ENABLE_SEND_ZC})
    set(CONFLUX_EXPERIMENTAL_RING_GROWTH ${CONFLUX_ENABLE_RING_GROWTH})
    set(CONFLUX_EXPERIMENTAL_IOPOLL_STORAGE_TEST ${CONFLUX_ENABLE_IOPOLL_STORAGE_TEST})
endif()

if(CONFLUX_BUILD_BENCHMARKS
    AND (CONFLUX_ENABLE_ASAN OR CONFLUX_ENABLE_UBSAN OR CONFLUX_ENABLE_TSAN)
    AND NOT CONFLUX_ALLOW_SANITIZED_BENCHMARKS)
    message(FATAL_ERROR
        "CONFLUX_BUILD_BENCHMARKS must stay OFF for sanitizer presets. "
        "Use perf-* presets for performance lanes, or set "
        "CONFLUX_ALLOW_SANITIZED_BENCHMARKS=ON for a local instrumented benchmark debug build.")
endif()

if(CONFLUX_PGO_GENERATE AND CONFLUX_PGO_PROFILE_DIR STREQUAL "")
    message(FATAL_ERROR
        "CONFLUX_PGO_GENERATE requires CONFLUX_PGO_PROFILE_DIR. "
        "Use a file pattern for Clang, e.g. /tmp/conflux-pgo/clang/%m-%p.profraw, "
        "or a directory for GCC, e.g. /tmp/conflux-pgo/gcc16.")
endif()

if((CONFLUX_PGO_GENERATE OR NOT CONFLUX_PGO_PROFILE_DIR STREQUAL "")
    AND (CONFLUX_ENABLE_ASAN OR CONFLUX_ENABLE_UBSAN OR CONFLUX_ENABLE_TSAN))
    message(FATAL_ERROR
        "PGO presets must not enable ASan/UBSan/TSan. Generate profiles from "
        "optimized, unsanitized builds and keep sanitizer coverage in the correctness lane.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ConfluxPresets)
conflux_apply_preset()
include(CompilerOptions)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(Python3_FIND_IMPLEMENTATIONS CPython)
set(Python3_FIND_STRATEGY LOCATION)
include(ConfluxInterfaceMode)
conflux_configure_interface_mode()

if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
    include(Dependencies)
    if(CONFLUX_BUILD_EXAMPLES OR CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_BENCHMARKS)
        find_package(OpenSSL)
        find_package(ZLIB)
        set(CONFLUX_HAS_TLS "false")
        if(OpenSSL_FOUND AND CONFLUX_ENABLE_TLS)
            set(CONFLUX_HAS_TLS "true")
        endif()
        set(CONFLUX_HAS_HTTP2 "false")
        if(CONFLUX_HAS_TLS STREQUAL "true" AND NGHTTP2_FOUND AND CONFLUX_ENABLE_HTTP2)
            set(CONFLUX_HAS_HTTP2 "true")
        endif()
        set(CONFLUX_HAS_HTTP3 "false")
        if(CONFLUX_HAS_TLS STREQUAL "true"
                AND NGTCP2_FOUND
                AND NGTCP2_CRYPTO_OSSL_FOUND
                AND NGHTTP3_FOUND
                AND CONFLUX_ENABLE_HTTP3)
            set(CONFLUX_HAS_HTTP3 "true")
        endif()
        set(CONFLUX_HAS_JSON "true")
        if(ZLIB_FOUND)
            set(CONFLUX_HAS_COMPRESS "true")
            set(CONFLUX_HAS_ZLIB "true")
        else()
            set(CONFLUX_HAS_COMPRESS "false")
            set(CONFLUX_HAS_ZLIB "false")
        endif()
        if(LIBDEFLATE_FOUND)
            set(CONFLUX_HAS_LIBDEFLATE "true")
        else()
            set(CONFLUX_HAS_LIBDEFLATE "false")
        endif()
        if(ZLIB_FOUND)
            set(CONFLUX_HAS_ZLIB_NG "false")
        elseif(ZLIB_NG_FOUND)
            set(CONFLUX_HAS_ZLIB_NG "true")
        else()
            set(CONFLUX_HAS_ZLIB_NG "false")
        endif()
        if(LIBISAL_FOUND)
            set(CONFLUX_HAS_ISAL "true")
        else()
            set(CONFLUX_HAS_ISAL "false")
        endif()
        if(BROTLI_FOUND)
            set(CONFLUX_HAS_BROTLI "true")
        else()
            set(CONFLUX_HAS_BROTLI "false")
        endif()
        if(ZSTD_FOUND)
            set(CONFLUX_HAS_ZSTD "true")
        else()
            set(CONFLUX_HAS_ZSTD "false")
        endif()
        set(CONFLUX_HAS_FILE_WATCH "true")
        set(CONFLUX_HAS_TEMPLATES "true")
        set(CONFLUX_HAS_TEMPLATES_WATCH "true")
        if(CONFLUX_ENABLE_METRICS)
            set(CONFLUX_HAS_METRICS "true")
        else()
            set(CONFLUX_HAS_METRICS "false")
        endif()
        if(LIBPQ_FOUND AND CONFLUX_ENABLE_DB)
            set(CONFLUX_HAS_DB "true")
        else()
            set(CONFLUX_HAS_DB "false")
        endif()
    endif()
    if(NOT DEFINED CONFLUX_HAS_DB)
        if(LIBPQ_FOUND AND CONFLUX_ENABLE_DB)
            set(CONFLUX_HAS_DB "true")
        else()
            set(CONFLUX_HAS_DB "false")
        endif()
    endif()
    if(CONFLUX_BUILD_TESTS)
        enable_testing()
    endif()
    conflux_add_header_interface_target()
    conflux_add_header_component_smoke_targets()
    conflux_add_header_examples_from_source_ids()
    conflux_add_header_test_compile_targets()
    conflux_add_header_compile_fail_tests()
    conflux_add_header_benchmark_compile_targets()

    add_library(conflux_core INTERFACE)
    target_link_libraries(conflux_core INTERFACE conflux_headers)
    target_compile_definitions(conflux_core INTERFACE CONFLUX_INTERFACE_HEADER=1)
    set_target_properties(conflux_core PROPERTIES EXPORT_NAME core)

    add_library(conflux_types INTERFACE)
    target_link_libraries(conflux_types INTERFACE conflux_headers)
    set_target_properties(conflux_types PROPERTIES EXPORT_NAME types)

    set(CONFLUX_HEADER_INSTALL_TARGETS conflux_headers conflux_core conflux_types)
    if(TARGET conflux_header_impl)
        list(APPEND CONFLUX_HEADER_INSTALL_TARGETS conflux_header_impl)
    endif()
    set(CONFLUX_PACKAGE_COMPONENTS core types)
    set(CONFLUX_PACKAGE_TARGETS conflux::core conflux::types)
    set(CONFLUX_INSTALL_MOCK_RUNTIME FALSE)
    if(CONFLUX_USE_MOCK_LIBURING)
        message(WARNING
            "conflux: mock liburing is enabled; runtime/http installed package components are disabled because they are compile-only in this build.")
    endif()
    set(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS FALSE)
    if(NOT CONFLUX_USE_MOCK_LIBURING AND TARGET PkgConfig::LIBURING)
        set(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS TRUE)
    endif()

    if(CONFLUX_WANT_JSON)
        add_library(conflux_json INTERFACE)
        target_link_libraries(conflux_json INTERFACE conflux_headers)
        set_target_properties(conflux_json PROPERTIES EXPORT_NAME json)
        list(APPEND CONFLUX_HEADER_INSTALL_TARGETS conflux_json)
        list(APPEND CONFLUX_PACKAGE_COMPONENTS json)
        list(APPEND CONFLUX_PACKAGE_TARGETS conflux::json)
    endif()

    if(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS
            AND (CONFLUX_WANT_HTTP_CORE OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER))
        add_library(conflux_net_http INTERFACE)
        target_link_libraries(conflux_net_http INTERFACE conflux_headers)
        set_target_properties(conflux_net_http PROPERTIES EXPORT_NAME http)
        list(APPEND CONFLUX_HEADER_INSTALL_TARGETS conflux_net_http)
        list(APPEND CONFLUX_PACKAGE_COMPONENTS http)
        list(APPEND CONFLUX_PACKAGE_TARGETS conflux::http)
    endif()

    add_library(conflux_file_io_sync INTERFACE)
    target_link_libraries(conflux_file_io_sync INTERFACE conflux_headers)
    set_target_properties(conflux_file_io_sync PROPERTIES EXPORT_NAME file_io_sync)
    list(APPEND CONFLUX_HEADER_INSTALL_TARGETS conflux_file_io_sync)
    list(APPEND CONFLUX_PACKAGE_COMPONENTS file_io_sync)
    list(APPEND CONFLUX_PACKAGE_TARGETS conflux::file_io_sync)

    if(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS)
        add_library(conflux_work INTERFACE)
        target_link_libraries(conflux_work INTERFACE conflux_headers)
        set_target_properties(conflux_work PROPERTIES EXPORT_NAME runtime)
        list(APPEND CONFLUX_HEADER_INSTALL_TARGETS conflux_work)
        list(APPEND CONFLUX_PACKAGE_COMPONENTS runtime)
        list(APPEND CONFLUX_PACKAGE_TARGETS conflux::runtime)
    endif()

    if(CONFLUX_HAS_DB STREQUAL "true")
        add_library(conflux_db INTERFACE)
        target_link_libraries(conflux_db INTERFACE conflux_headers)
        if(TARGET PkgConfig::LIBPQ)
            target_link_libraries(conflux_db INTERFACE PkgConfig::LIBPQ)
        endif()
        set_target_properties(conflux_db PROPERTIES EXPORT_NAME db)
        list(APPEND CONFLUX_HEADER_INSTALL_TARGETS conflux_db)
        list(APPEND CONFLUX_PACKAGE_COMPONENTS db)
        list(APPEND CONFLUX_PACKAGE_TARGETS conflux::db)
    endif()

    set(CONFLUX_PUBLIC_HPP_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/public-hpp/conflux")
    file(MAKE_DIRECTORY "${CONFLUX_PUBLIC_HPP_DIR}")
    file(WRITE "${CONFLUX_PUBLIC_HPP_DIR}/conflux.hpp" "#pragma once\n#include <conflux.hxx>\n")
    file(WRITE "${CONFLUX_PUBLIC_HPP_DIR}/config.hpp" "#pragma once\n#include <conflux/config.hxx>\n")
    file(WRITE "${CONFLUX_PUBLIC_HPP_DIR}/types.hpp" "#pragma once\n#include <conflux/types.hxx>\n")
    file(WRITE "${CONFLUX_PUBLIC_HPP_DIR}/json.hpp" "#pragma once\n#include <conflux/json.hxx>\n")
    file(WRITE "${CONFLUX_PUBLIC_HPP_DIR}/file_io_sync.hpp" "#pragma once\n#include <conflux/file_io_sync.hxx>\n")
    if(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS)
        file(WRITE "${CONFLUX_PUBLIC_HPP_DIR}/http.hpp" "#pragma once\n#include <conflux/http.hxx>\n")
        file(WRITE "${CONFLUX_PUBLIC_HPP_DIR}/runtime.hpp" "#pragma once\n#include <conflux/work.hxx>\n")
    endif()
    if(CONFLUX_HAS_DB STREQUAL "true")
        file(WRITE "${CONFLUX_PUBLIC_HPP_DIR}/db.hpp" "#pragma once\n#include <conflux/db.hxx>\n")
    endif()

    set(CONFLUX_INSTALL_NEEDS_LIBURING ${CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS})
    set(CONFLUX_INSTALL_NEEDS_XXHASH FALSE)
    if(CONFLUX_WANT_JSON)
        set(CONFLUX_INSTALL_NEEDS_XXHASH TRUE)
    endif()
    set(CONFLUX_INSTALL_NEEDS_LIBPQ FALSE)
    if(CONFLUX_HAS_DB STREQUAL "true")
        set(CONFLUX_INSTALL_NEEDS_LIBPQ TRUE)
    endif()
    set(CONFLUX_INSTALL_NEEDS_OPENSSL FALSE)
    set(CONFLUX_INSTALL_NEEDS_ZLIB FALSE)
    set(CONFLUX_INSTALL_NEEDS_LIBDEFLATE FALSE)
    set(CONFLUX_INSTALL_NEEDS_ZLIB_NG FALSE)
    set(CONFLUX_INSTALL_NEEDS_ISAL FALSE)
    set(CONFLUX_INSTALL_NEEDS_BROTLI FALSE)
    set(CONFLUX_INSTALL_NEEDS_ZSTD FALSE)
    set(CONFLUX_INSTALL_NEEDS_NGHTTP2 FALSE)
    set(CONFLUX_INSTALL_NEEDS_NGTCP2 FALSE)
    set(CONFLUX_INSTALL_NEEDS_NGHTTP3 FALSE)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/conflux-config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(TARGETS ${CONFLUX_HEADER_INSTALL_TARGETS}
        EXPORT confluxTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES
        ${CONFLUX_SRC_ROOT}/conflux/detail/discard.hxx
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/detail
    )
    if(CONFLUX_USE_MOCK_LIBURING)
        install(FILES
            "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux.hxx"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        install(FILES
            "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/config.hxx"
            "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/features.hxx"
            "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/types.hxx"
            "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/json.hxx"
            "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/file_io_sync.hxx"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux)
        install(DIRECTORY "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/detail/generated/"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/detail/generated)
    elseif(CONFLUX_HAS_DB STREQUAL "true")
        install(DIRECTORY "${CONFLUX_GENERATED_INCLUDE_DIR}/"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    else()
        install(DIRECTORY "${CONFLUX_GENERATED_INCLUDE_DIR}/"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            PATTERN "pg" EXCLUDE
            PATTERN "pg.hxx" EXCLUDE
            PATTERN "db.hxx" EXCLUDE)
    endif()
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
    )
    install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/public-hpp/"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT confluxTargets
        NAMESPACE conflux::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
    )
    return()
endif()

include(Dependencies)

# ---------------------------------------------------------------------------
# Optional dependencies
# ---------------------------------------------------------------------------

find_package(OpenSSL)
find_package(ZLIB)

# ---- OpenSSL / TLS ---------------------------------------------------------
if(OpenSSL_FOUND AND CONFLUX_ENABLE_TLS)
    set(CONFLUX_HAS_TLS "true")
    message(STATUS "conflux: TLS enabled (OpenSSL ${OPENSSL_VERSION})")
else()
    set(CONFLUX_HAS_TLS "false")
    if(NOT OpenSSL_FOUND)
        message(STATUS "conflux: TLS disabled (OpenSSL not found)")
    else()
        message(STATUS "conflux: TLS disabled (CONFLUX_ENABLE_TLS=OFF)")
    endif()
endif()

# ---- zlib / gzip -----------------------------------------------------------
set(CONFLUX_GZIP_BACKENDS "")
if(ZLIB_FOUND AND CONFLUX_ENABLE_ZLIB)
    set(CONFLUX_HAS_ZLIB "true")
    list(APPEND CONFLUX_GZIP_BACKENDS "zlib")
else()
    set(CONFLUX_HAS_ZLIB "false")
endif()
if(LIBDEFLATE_FOUND AND CONFLUX_ENABLE_LIBDEFLATE)
    list(APPEND CONFLUX_GZIP_BACKENDS "libdeflate")
endif()
if(ZLIB_NG_FOUND AND CONFLUX_ENABLE_ZLIB_NG)
    list(APPEND CONFLUX_GZIP_BACKENDS "zlib-ng")
endif()
if(LIBISAL_FOUND AND CONFLUX_ENABLE_ISAL)
    list(APPEND CONFLUX_GZIP_BACKENDS "isa-l")
endif()

if(CONFLUX_GZIP_BACKENDS)
    set(CONFLUX_HAS_COMPRESS "true")
    list(JOIN CONFLUX_GZIP_BACKENDS ", " CONFLUX_GZIP_BACKENDS_STR)
    message(STATUS "conflux: gzip enabled (${CONFLUX_GZIP_BACKENDS_STR})")
else()
    set(CONFLUX_HAS_COMPRESS "false")
    message(STATUS "conflux: gzip disabled (no gzip backend found)")
endif()

# ---- optional gzip backends ------------------------------------------------
if(LIBDEFLATE_FOUND AND CONFLUX_ENABLE_LIBDEFLATE)
    set(CONFLUX_HAS_LIBDEFLATE "true")
    message(STATUS "conflux: libdeflate enabled")
else()
    set(CONFLUX_HAS_LIBDEFLATE "false")
    if(NOT LIBDEFLATE_FOUND)
        message(STATUS "conflux: libdeflate disabled (libdeflate not found)")
    else()
        message(STATUS "conflux: libdeflate disabled (CONFLUX_ENABLE_LIBDEFLATE=OFF)")
    endif()
endif()

if(ZLIB_NG_FOUND AND CONFLUX_ENABLE_ZLIB_NG)
    set(CONFLUX_HAS_ZLIB_NG "true")
    message(STATUS "conflux: zlib-ng enabled")
else()
    set(CONFLUX_HAS_ZLIB_NG "false")
    if(NOT ZLIB_NG_FOUND)
        message(STATUS "conflux: zlib-ng disabled (zlib-ng not found)")
    else()
        message(STATUS "conflux: zlib-ng disabled (CONFLUX_ENABLE_ZLIB_NG=OFF)")
    endif()
endif()

if(LIBISAL_FOUND AND CONFLUX_ENABLE_ISAL)
    set(CONFLUX_HAS_ISAL "true")
    message(STATUS "conflux: isa-l enabled")
else()
    set(CONFLUX_HAS_ISAL "false")
    if(NOT LIBISAL_FOUND)
        message(STATUS "conflux: isa-l disabled (libisal not found)")
    else()
        message(STATUS "conflux: isa-l disabled (CONFLUX_ENABLE_ISAL=OFF)")
    endif()
endif()

# ---- brotli ----------------------------------------------------------------
if(BROTLI_FOUND AND CONFLUX_ENABLE_BROTLI)
    set(CONFLUX_HAS_BROTLI "true")
    message(STATUS "conflux: brotli enabled")
else()
    set(CONFLUX_HAS_BROTLI "false")
    if(NOT BROTLI_FOUND)
        message(STATUS "conflux: brotli disabled (libbrotlienc/libbrotlidec not found)")
    else()
        message(STATUS "conflux: brotli disabled (CONFLUX_ENABLE_BROTLI=OFF)")
    endif()
endif()

# ---- zstd ------------------------------------------------------------------
if(ZSTD_FOUND AND CONFLUX_ENABLE_ZSTD)
    set(CONFLUX_HAS_ZSTD "true")
    message(STATUS "conflux: zstd enabled")
else()
    set(CONFLUX_HAS_ZSTD "false")
    if(NOT ZSTD_FOUND)
        message(STATUS "conflux: zstd disabled (libzstd not found)")
    else()
        message(STATUS "conflux: zstd disabled (CONFLUX_ENABLE_ZSTD=OFF)")
    endif()
endif()

# ---- Prometheus metrics (no external dep) ----------------------------------
if(CONFLUX_ENABLE_METRICS)
    set(CONFLUX_HAS_METRICS "true")
    message(STATUS "conflux: metrics enabled")
else()
    set(CONFLUX_HAS_METRICS "false")
    message(STATUS "conflux: metrics disabled")
endif()

# ---- HTTP/2 (requires nghttp2 + TLS) ---------------------------------------
if(NGHTTP2_FOUND AND CONFLUX_HAS_TLS STREQUAL "true" AND CONFLUX_ENABLE_HTTP2)
    set(CONFLUX_HAS_HTTP2 "true")
    message(STATUS "conflux: HTTP/2 enabled (nghttp2)")
else()
    set(CONFLUX_HAS_HTTP2 "false")
    if(NOT NGHTTP2_FOUND)
        message(STATUS "conflux: HTTP/2 disabled (libnghttp2 not found)")
    elseif(NOT CONFLUX_HAS_TLS STREQUAL "true")
        message(STATUS "conflux: HTTP/2 disabled (TLS required)")
    else()
        message(STATUS "conflux: HTTP/2 disabled (CONFLUX_ENABLE_HTTP2=OFF)")
    endif()
endif()

# ---- HTTP/3 (requires ngtcp2 + nghttp3 + OpenSSL 3.5+ + TLS) ---------------
set(CONFLUX_HAS_HTTP3_OPENSSL "false")
if(OpenSSL_FOUND AND OPENSSL_VERSION VERSION_GREATER_EQUAL "3.5.0")
    set(CONFLUX_HAS_HTTP3_OPENSSL "true")
endif()

if(NGTCP2_FOUND AND NGTCP2_CRYPTO_OSSL_FOUND AND NGHTTP3_FOUND
        AND CONFLUX_HAS_TLS STREQUAL "true"
        AND CONFLUX_HAS_HTTP3_OPENSSL STREQUAL "true"
        AND CONFLUX_ENABLE_HTTP3)
    set(CONFLUX_HAS_HTTP3 "true")
    message(STATUS "conflux: HTTP/3 enabled (ngtcp2 ${NGTCP2_VERSION}, nghttp3 ${NGHTTP3_VERSION})")
else()
    set(CONFLUX_HAS_HTTP3 "false")
    if(NOT NGTCP2_FOUND)
        message(STATUS "conflux: HTTP/3 disabled (libngtcp2 not found)")
    elseif(NOT NGTCP2_CRYPTO_OSSL_FOUND)
        message(STATUS "conflux: HTTP/3 disabled (libngtcp2_crypto_ossl not found)")
    elseif(NOT NGHTTP3_FOUND)
        message(STATUS "conflux: HTTP/3 disabled (libnghttp3 not found)")
    elseif(NOT CONFLUX_HAS_TLS STREQUAL "true")
        message(STATUS "conflux: HTTP/3 disabled (TLS required)")
    elseif(NOT CONFLUX_HAS_HTTP3_OPENSSL STREQUAL "true")
        message(STATUS "conflux: HTTP/3 disabled (OpenSSL ${OPENSSL_VERSION} < 3.5.0)")
    else()
        message(STATUS "conflux: HTTP/3 disabled (CONFLUX_ENABLE_HTTP3=OFF)")
    endif()
endif()

# ---- Postgres (libpq) ------------------------------------------------------
if(LIBPQ_FOUND AND CONFLUX_ENABLE_DB)
    set(CONFLUX_HAS_DB "true")
    message(STATUS "conflux: db enabled (libpq ${LIBPQ_VERSION})")
else()
    set(CONFLUX_HAS_DB "false")
    if(NOT LIBPQ_FOUND)
        message(STATUS "conflux: db disabled (libpq not found)")
    else()
        message(STATUS "conflux: db disabled (CONFLUX_ENABLE_DB=OFF)")
    endif()
endif()

if(CONFLUX_WANT_FILE_WATCH)
    set(CONFLUX_HAS_FILE_WATCH "true")
else()
    set(CONFLUX_HAS_FILE_WATCH "false")
endif()
if(CONFLUX_WANT_TEMPLATES)
    set(CONFLUX_HAS_TEMPLATES "true")
else()
    set(CONFLUX_HAS_TEMPLATES "false")
endif()
if(CONFLUX_WANT_TEMPLATES_WATCH)
    set(CONFLUX_HAS_TEMPLATES_WATCH "true")
else()
    set(CONFLUX_HAS_TEMPLATES_WATCH "false")
endif()
if(CONFLUX_WANT_JSON)
    set(CONFLUX_HAS_JSON "true")
else()
    set(CONFLUX_HAS_JSON "false")
endif()
if(CONFLUX_USE_MOCK_LIBURING)
    set(CONFLUX_BUILD_INFO_MOCK_LIBURING "true")
else()
    set(CONFLUX_BUILD_INFO_MOCK_LIBURING "false")
endif()

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE CONFLUX_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT CONFLUX_GIT_COMMIT)
    set(CONFLUX_GIT_COMMIT "unknown")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CONFLUX_STDLIB_NAME "libstdc++")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CONFLUX_STDLIB_NAME "libc++/libstdc++")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CONFLUX_STDLIB_NAME "msvc-stl")
else()
    set(CONFLUX_STDLIB_NAME "unknown")
endif()
set(CONFLUX_STDLIB_VERSION "${CMAKE_CXX_COMPILER_VERSION}")

# Generate the conflux.features module from the template.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/conflux_features.cxx.in"
    "${CMAKE_CURRENT_BINARY_DIR}/src/conflux_features.cxx"
    @ONLY
)

# ---------------------------------------------------------------------------
# Hardware feature detection (needed before target definitions)
# ---------------------------------------------------------------------------

set(CMAKE_REQUIRED_FLAGS "-maes -mpclmul -mssse3 -msse4.1")
set(_AESNI_TEST_CODE "
#include <wmmintrin.h>
#include <tmmintrin.h>
#include <smmintrin.h>
int main() {
    __m128i a = _mm_setzero_si128();
    __m128i b = _mm_aesenc_si128(a, a);
    __m128i c = _mm_clmulepi64_si128(a, b, 0x00);
    __m128i d = _mm_shuffle_epi8(c, a);
    (void)_mm_test_all_zeros(d, d);
    return 0;
}")
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("${_AESNI_TEST_CODE}" CONFLUX_HAS_AESNI)
unset(CMAKE_REQUIRED_FLAGS)

set(_CONFLUX_DISCARD_TEST_CODE "
int f();
int main() {
    auto _ = f();
    return 0;
}")
set(CMAKE_REQUIRED_FLAGS "-Werror -Wpedantic")
check_cxx_source_compiles("${_CONFLUX_DISCARD_TEST_CODE}" CONFLUX_HAS_WARNING_CLEAN_AUTO_UNDERSCORE_DISCARD)
unset(CMAKE_REQUIRED_FLAGS)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(conflux_options INTERFACE)
conflux_apply_compiler_options(conflux_options)
target_include_directories(conflux_options INTERFACE
    "$<BUILD_INTERFACE:${CONFLUX_SRC_ROOT}>"
    "$<BUILD_INTERFACE:${CONFLUX_GENERATED_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:include>"
    "$<INSTALL_INTERFACE:include/conflux/modules>"
)
target_compile_definitions(conflux_options INTERFACE
    CONFLUX_HEADER_USE_IMPORT_STD=0
    CONFLUX_HEADER_USE_IMPORT_STD_COMPAT=0
    CONFLUX_HEADER_USE_MODULE_IMPORTS=0
    CONFLUX_MODULE_USE_IMPORT_STD=$<BOOL:${CMAKE_CXX_MODULE_STD}>
    CONFLUX_MODULE_USE_IMPORT_STD_COMPAT=0
    CONFLUX_MODULE_USE_IMPORTS=0
    CONFLUX_MODULE_REEXPORT_IMPORTS=0
    CONFLUX_ENABLE_EXPERIMENTAL=$<BOOL:${CONFLUX_ENABLE_EXPERIMENTAL}>
    CONFLUX_ENABLE_RECV_BUNDLE=$<BOOL:${CONFLUX_ENABLE_RECV_BUNDLE}>
    CONFLUX_ENABLE_RECV_INCREMENTAL_BUF=$<BOOL:${CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF}>
    CONFLUX_ENABLE_SEND_ZC=$<BOOL:${CONFLUX_EXPERIMENTAL_SEND_ZC}>
    CONFLUX_ENABLE_RING_GROWTH=$<BOOL:${CONFLUX_EXPERIMENTAL_RING_GROWTH}>
    CONFLUX_ENABLE_IOPOLL_STORAGE_TEST=$<BOOL:${CONFLUX_EXPERIMENTAL_IOPOLL_STORAGE_TEST}>
    CONFLUX_USE_MOCK_LIBURING=$<BOOL:${CONFLUX_USE_MOCK_LIBURING}>
    CONFLUX_HAS_WARNING_CLEAN_AUTO_UNDERSCORE_DISCARD=$<BOOL:${CONFLUX_HAS_WARNING_CLEAN_AUTO_UNDERSCORE_DISCARD}>
    CONFLUX_BUILD_VERSION="${PROJECT_VERSION}"
    CONFLUX_BUILD_GIT_COMMIT="${CONFLUX_GIT_COMMIT}"
    CONFLUX_BUILD_COMPILER="${CMAKE_CXX_COMPILER_ID}"
    CONFLUX_BUILD_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    CONFLUX_BUILD_STDLIB="${CONFLUX_STDLIB_NAME}"
    CONFLUX_BUILD_STDLIB_VERSION="${CONFLUX_STDLIB_VERSION}"
    CONFLUX_BUILD_TYPE_VALUE="${CMAKE_BUILD_TYPE}"
    CONFLUX_BUILD_INTERFACE_MODE="${CONFLUX_INTERFACE_MODE}"
    CONFLUX_BUILD_FEATURE_SET="${CONFLUX_FEATURE_SET}"
)
if(CONFLUX_ENABLE_EXPERIMENTAL)
    message(STATUS "conflux: experimental features enabled")
else()
    message(STATUS "conflux: experimental features disabled")
endif()
if(CONFLUX_ENABLE_RECV_BUNDLE)
    message(STATUS "conflux: recv bundle enabled")
else()
    message(STATUS "conflux: recv bundle disabled")
endif()
if(CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF)
    message(STATUS "conflux: experimental incremental buf ring enabled")
else()
    message(STATUS "conflux: experimental incremental buf ring disabled")
endif()
if(CONFLUX_EXPERIMENTAL_SEND_ZC)
    message(STATUS "conflux: experimental SEND_ZC enabled")
else()
    message(STATUS "conflux: experimental SEND_ZC disabled")
endif()
if(CONFLUX_EXPERIMENTAL_RING_GROWTH)
    message(STATUS "conflux: experimental ring growth enabled")
else()
    message(STATUS "conflux: experimental ring growth disabled")
endif()
if(CONFLUX_EXPERIMENTAL_IOPOLL_STORAGE_TEST)
    message(STATUS "conflux: experimental IOPOLL storage test enabled")
else()
    message(STATUS "conflux: experimental IOPOLL storage test disabled")
endif()
if(CONFLUX_ENABLE_HTTP_TRACE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(conflux_options INTERFACE CONFLUX_HTTP_TRACE=1)
    message(STATUS "conflux: HTTP trace instrumentation enabled for Debug")
elseif(CONFLUX_ENABLE_HTTP_TRACE)
    message(STATUS "conflux: HTTP trace requested but ignored outside Debug builds")
endif()

add_library(conflux_types STATIC)
target_compile_features(conflux_types PUBLIC cxx_std_23)
target_sources(conflux_types
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/types.cppm
        ${CONFLUX_SRC_ROOT}/types_api.cxx
)
target_link_libraries(conflux_types
    PRIVATE conflux_options
)

# First slice of the public component model: a liburing-free core target.
# `conflux_types` remains the concrete module carrier; `conflux_core` is the
# stable package-facing dependency anchor for consumers that only need core
# vocabulary/error types.
add_library(conflux_core INTERFACE)
target_compile_features(conflux_core INTERFACE cxx_std_23)
target_link_libraries(conflux_core INTERFACE conflux_types)
if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    target_compile_definitions(conflux_core INTERFACE CONFLUX_INTERFACE_MODULE=1)
endif()

if(CONFLUX_WANT_CRYPTO)
add_library(conflux_crypto STATIC)
target_sources(conflux_crypto
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/crypto.cxx
)
target_link_libraries(conflux_crypto
    PRIVATE conflux_options
    PUBLIC  conflux_types
)
if(CONFLUX_HAS_AESNI)
    message(STATUS "conflux: AES-NI + PCLMUL + SSSE3 path enabled")
    target_compile_definitions(conflux_crypto PRIVATE CONFLUX_CRYPTO_USE_AESNI=1)
    target_sources(conflux_crypto PRIVATE ${CONFLUX_SRC_ROOT}/crypto_aesni.cxx)
    set_source_files_properties(${CONFLUX_SRC_ROOT}/crypto_aesni.cxx PROPERTIES
        COMPILE_OPTIONS "-maes;-mpclmul;-mssse3;-msse4.1"
    )
endif()
endif()

if(CONFLUX_WANT_JSON OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_json_boundary STATIC)
target_sources(conflux_json_boundary
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/json_boundary.cxx
)
target_link_libraries(conflux_json_boundary
    PUBLIC  conflux_types
    PRIVATE conflux_options
)
endif()

if(CONFLUX_WANT_JSON)
add_library(conflux_json STATIC)
target_compile_features(conflux_json PUBLIC cxx_std_23)
target_sources(conflux_json
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/json.cppm
        ${CONFLUX_SRC_ROOT}/json_api.cxx
)
target_sources(conflux_json
    PRIVATE
        ${CONFLUX_SRC_ROOT}/json_storage.cxx
        ${CONFLUX_SRC_ROOT}/json_number.cxx
        ${CONFLUX_SRC_ROOT}/json_reader.cxx
        ${CONFLUX_SRC_ROOT}/json_dom.cxx
        ${CONFLUX_SRC_ROOT}/json_lookup.cxx
        ${CONFLUX_SRC_ROOT}/json_dump.cxx
        ${CONFLUX_SRC_ROOT}/json_parse.cxx
        ${CONFLUX_SRC_ROOT}/json_builder.cxx
        ${CONFLUX_SRC_ROOT}/json_stream.cxx
)
target_link_libraries(conflux_json
    PUBLIC  conflux_types
    PRIVATE conflux_options PkgConfig::XXHASH
)

add_library(conflux_json_native_provider STATIC)
target_sources(conflux_json_native_provider
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/json_native_provider.cxx
)
target_link_libraries(conflux_json_native_provider
    PRIVATE conflux_options
    PUBLIC  conflux_json_boundary conflux_json
)
endif()

if(CONFLUX_WANT_FILE_WATCH AND NOT CONFLUX_WANT_FILE_IO)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_FILE_WATCH requires CONFLUX_BUILD_FILE_IO")
endif()
if(CONFLUX_WANT_TEMPLATES AND NOT TARGET conflux_json)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_TEMPLATES requires CONFLUX_BUILD_JSON")
endif()
if(CONFLUX_WANT_JSON_FILE AND NOT TARGET conflux_json)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_JSON_FILE requires CONFLUX_BUILD_JSON")
endif()
if(CONFLUX_WANT_JSON_FILE AND NOT CONFLUX_WANT_FILE_IO_SYNC)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_JSON_FILE requires CONFLUX_BUILD_FILE_IO_SYNC")
endif()
if(CONFLUX_WANT_TEMPLATES_WATCH AND NOT CONFLUX_WANT_TEMPLATES)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_TEMPLATES_WATCH requires CONFLUX_BUILD_TEMPLATES")
endif()
if(CONFLUX_WANT_TEMPLATES_WATCH AND NOT CONFLUX_WANT_FILE_WATCH)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_TEMPLATES_WATCH requires CONFLUX_BUILD_FILE_WATCH")
endif()
if((CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER) AND NOT CONFLUX_WANT_HTTP_CORE)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_HTTP_JSON/CONFLUX_BUILD_HTTP_SERVER requires CONFLUX_BUILD_HTTP_CORE")
endif()
if((CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER) AND NOT TARGET conflux_json_boundary)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_HTTP_JSON/CONFLUX_BUILD_HTTP_SERVER requires conflux_json_boundary")
endif()

set(CONFLUX_HTTP_ROUTER_STACK_REQUESTED FALSE)
if(CONFLUX_WANT_HTTP_ROUTER OR CONFLUX_WANT_HTTP_SERVER
        OR CONFLUX_WANT_HTTP_POLICY OR CONFLUX_WANT_HTTP_AUTH
        OR CONFLUX_WANT_HTTP_COMPRESSION OR CONFLUX_WANT_HTTP_PROXY
        OR CONFLUX_WANT_HTTP_OBSERVABILITY OR CONFLUX_WANT_HTTP_OPENAPI
        OR CONFLUX_WANT_HTTP_VHOST)
    set(CONFLUX_HTTP_ROUTER_STACK_REQUESTED TRUE)
endif()

set(CONFLUX_HTTP_CLIENT_STACK_REQUESTED FALSE)
if(CONFLUX_WANT_HTTP_CLIENT OR CONFLUX_WANT_HTTP_PROXY OR CONFLUX_WANT_HTTP_SERVER)
    set(CONFLUX_HTTP_CLIENT_STACK_REQUESTED TRUE)
endif()

set(CONFLUX_HTTP_STATIC_SURFACE_REQUESTED FALSE)
if(CONFLUX_WANT_HTTP_STATIC OR CONFLUX_HTTP_ROUTER_STACK_REQUESTED)
    set(CONFLUX_HTTP_STATIC_SURFACE_REQUESTED TRUE)
endif()

set(CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED FALSE)
if(CONFLUX_WANT_HTTP_REALTIME OR CONFLUX_HTTP_ROUTER_STACK_REQUESTED)
    set(CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED TRUE)
endif()

if(CONFLUX_HTTP_STATIC_SURFACE_REQUESTED AND NOT CONFLUX_WANT_RUNTIME)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_HTTP_STATIC requires CONFLUX_BUILD_RUNTIME")
endif()

if(CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED AND NOT CONFLUX_WANT_HTTP_CORE)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_HTTP_REALTIME/HTTP router components require CONFLUX_BUILD_HTTP_CORE")
endif()
if(CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED AND NOT CONFLUX_WANT_CRYPTO)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_HTTP_REALTIME/HTTP router components require CONFLUX_BUILD_CRYPTO")
endif()

if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED AND NOT CONFLUX_WANT_HTTP_CORE)
    message(FATAL_ERROR "conflux: HTTP router/middleware component flags require CONFLUX_BUILD_HTTP_CORE")
endif()
if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED AND NOT CONFLUX_WANT_RUNTIME)
    message(FATAL_ERROR "conflux: HTTP router/middleware component flags require CONFLUX_BUILD_RUNTIME")
endif()
if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED AND NOT CONFLUX_WANT_FILE_IO)
    message(FATAL_ERROR "conflux: HTTP router/middleware component flags require CONFLUX_BUILD_FILE_IO")
endif()
if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED AND NOT CONFLUX_WANT_SOCKET_IO)
    message(FATAL_ERROR "conflux: HTTP router/middleware component flags require CONFLUX_BUILD_SOCKET_IO")
endif()
if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED AND NOT CONFLUX_WANT_CRYPTO)
    message(FATAL_ERROR "conflux: HTTP router/middleware component flags require CONFLUX_BUILD_CRYPTO")
endif()

if(CONFLUX_HTTP_CLIENT_STACK_REQUESTED AND NOT CONFLUX_WANT_HTTP_CORE)
    message(FATAL_ERROR "conflux: HTTP client/proxy component flags require CONFLUX_BUILD_HTTP_CORE")
endif()
if(CONFLUX_HTTP_CLIENT_STACK_REQUESTED AND NOT CONFLUX_WANT_RUNTIME)
    message(FATAL_ERROR "conflux: HTTP client/proxy component flags require CONFLUX_BUILD_RUNTIME")
endif()
if(CONFLUX_HTTP_CLIENT_STACK_REQUESTED AND NOT CONFLUX_WANT_SOCKET_IO)
    message(FATAL_ERROR "conflux: HTTP client/proxy component flags require CONFLUX_BUILD_SOCKET_IO")
endif()
if(CONFLUX_HTTP_CLIENT_STACK_REQUESTED AND NOT CONFLUX_WANT_DNS)
    message(FATAL_ERROR "conflux: HTTP client/proxy component flags require CONFLUX_BUILD_DNS")
endif()
if(CONFLUX_HAS_TLS STREQUAL "true" AND (CONFLUX_HTTP_CLIENT_STACK_REQUESTED OR CONFLUX_WANT_SMTP)
        AND NOT CONFLUX_WANT_FILE_IO)
    message(FATAL_ERROR "conflux: TLS HTTP client/proxy/SMTP components require CONFLUX_BUILD_FILE_IO")
endif()
if(CONFLUX_WANT_SMTP AND NOT CONFLUX_WANT_RUNTIME)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_BUILD_RUNTIME")
endif()
if(CONFLUX_WANT_SMTP AND NOT CONFLUX_WANT_DNS)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_BUILD_DNS")
endif()
if(CONFLUX_WANT_SMTP AND NOT CONFLUX_WANT_CRYPTO)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_BUILD_CRYPTO")
endif()
if(CONFLUX_WANT_SMTP AND NOT CONFLUX_WANT_SOCKET_IO)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_BUILD_SOCKET_IO")
endif()
if(CONFLUX_WANT_SMTP
        AND CONFLUX_HAS_TLS STREQUAL "false"
        AND CONFLUX_BUILD_SMTP STREQUAL "AUTO")
    set(CONFLUX_WANT_SMTP FALSE)
    set(CONFLUX_WANT_SMTP FALSE CACHE INTERNAL "Resolved conflux component flag" FORCE)
    message(STATUS "conflux: SMTP disabled (TLS unavailable)")
elseif(CONFLUX_WANT_SMTP AND CONFLUX_HAS_TLS STREQUAL "false")
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_ENABLE_TLS and OpenSSL")
endif()

if(CONFLUX_WANT_FILE_MAP AND NOT CONFLUX_WANT_FILE_IO_SYNC)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_FILE_MAP requires CONFLUX_BUILD_FILE_IO_SYNC")
endif()
if(CONFLUX_WANT_FILE_IO AND NOT CONFLUX_WANT_FILE_IO_SYNC)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_FILE_IO requires CONFLUX_BUILD_FILE_IO_SYNC")
endif()
if(CONFLUX_WANT_FILE_IO AND NOT CONFLUX_WANT_FILE_MAP)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_FILE_IO requires CONFLUX_BUILD_FILE_MAP")
endif()
if(CONFLUX_WANT_TEMPLATES AND NOT CONFLUX_WANT_FILE_IO_SYNC)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_TEMPLATES requires CONFLUX_BUILD_FILE_IO_SYNC")
endif()
if(CONFLUX_WANT_PROCESS AND NOT CONFLUX_WANT_RUNTIME)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_PROCESS requires CONFLUX_BUILD_RUNTIME")
endif()

if(CONFLUX_WANT_FILE_IO_SYNC)
add_library(conflux_file_io_sync STATIC)
target_compile_features(conflux_file_io_sync PUBLIC cxx_std_23)
target_sources(conflux_file_io_sync
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/file_io/file_io_sync.cxx
)
target_link_libraries(conflux_file_io_sync
    PRIVATE conflux_options
    PUBLIC  conflux_types
)
endif() # CONFLUX_WANT_FILE_IO_SYNC

if(CONFLUX_WANT_FILE_MAP)
add_library(conflux_file_map STATIC)
target_sources(conflux_file_map
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/file_io/file_map.cxx
)
target_link_libraries(conflux_file_map
    PRIVATE conflux_options
    PUBLIC  conflux_file_io_sync
)
endif() # CONFLUX_WANT_FILE_MAP

if(CONFLUX_WANT_JSON_FILE)
add_library(conflux_json_file STATIC)
target_compile_features(conflux_json_file PUBLIC cxx_std_23)
target_sources(conflux_json_file
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/json_file.cxx
)
target_link_libraries(conflux_json_file
    PRIVATE conflux_options
    PUBLIC  conflux_json conflux_file_io_sync
)
endif() # CONFLUX_WANT_JSON_FILE

if(CONFLUX_WANT_HTTP_CORE OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
    add_library(conflux_http_core STATIC)
    target_sources(conflux_http_core
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CONFLUX_SRC_ROOT}"
            FILES
            ${CONFLUX_SRC_ROOT}/net/http_types.cxx
            ${CONFLUX_SRC_ROOT}/net/http_request.cxx
            ${CONFLUX_SRC_ROOT}/net/server_types.cxx
    )
    target_link_libraries(conflux_http_core
        PRIVATE conflux_options
        PUBLIC  conflux_types
    )
endif()

if(CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
    add_library(conflux_http_json STATIC)
    target_sources(conflux_http_json
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CONFLUX_SRC_ROOT}"
            FILES ${CONFLUX_SRC_ROOT}/net/http_json.cxx
    )
    target_link_libraries(conflux_http_json
        PRIVATE conflux_options
        PUBLIC  conflux_http_core
        PUBLIC  conflux_json_boundary
    )
endif()

if(CONFLUX_NEEDS_RUNTIME)

add_library(conflux_uring STATIC)
target_sources(conflux_uring
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/uring/uring.cxx
        ${CONFLUX_SRC_ROOT}/uring/flow.cxx
        ${CONFLUX_SRC_ROOT}/uring/uring_completion.cxx
        ${CONFLUX_SRC_ROOT}/uring/uring_handle.cxx
)
target_link_libraries(conflux_uring
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  PkgConfig::LIBURING
)

add_library(conflux_uring_timeout STATIC)
target_sources(conflux_uring_timeout
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/uring/uring_timeout.cxx
)
target_link_libraries(conflux_uring_timeout
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
    PUBLIC  conflux_uring
    PUBLIC  PkgConfig::LIBURING
)

# Probes placed after include(Dependencies) so PkgConfig::LIBURING is available.
include(CheckCXXSourceCompiles)
include(CheckCXXSourceRuns)
set(CMAKE_REQUIRED_LIBRARIES PkgConfig::LIBURING)
check_cxx_source_compiles("
#include <liburing.h>
int main() {
    auto* fn = &io_uring_resize_rings;
    (void)fn;
    return 0;
}
" CONFLUX_HAVE_IO_URING_RESIZE_RINGS)
check_cxx_source_compiles("
#include <liburing.h>
int main() {
    auto* fn = &io_uring_clone_buffers_offset;
    (void)fn;
    return 0;
}
" CONFLUX_HAVE_IO_URING_CLONE_BUFFERS_OFFSET)
check_cxx_source_runs("
#include <liburing.h>
int main() {
    io_uring ring{};
    if (io_uring_queue_init(8, &ring, 0) < 0) {
        return 1;
    }
    int err = 0;
    io_uring_buf_ring* br = io_uring_setup_buf_ring(&ring, 8, 0, IOU_PBUF_RING_INC, &err);
    if (br == nullptr) {
        io_uring_queue_exit(&ring);
        return 1;
    }
    io_uring_free_buf_ring(&ring, br, 8, 0);
    io_uring_queue_exit(&ring);
    return 0;
}
" CONFLUX_RUNTIME_HAS_IOU_PBUF_RING_INC)
if(CONFLUX_HAVE_IO_URING_RESIZE_RINGS)
    check_cxx_source_runs("
#include <liburing.h>
int main() {
    io_uring ring{};
    io_uring_params init{};
    init.flags = IORING_SETUP_DEFER_TASKRUN;
    if (io_uring_queue_init_params(8, &ring, &init) < 0) {
        return 1;
    }
    io_uring_params resize{};
    resize.flags = IORING_SETUP_CQSIZE;
    resize.sq_entries = 8;
    resize.cq_entries = 16;
    int rc = io_uring_resize_rings(&ring, &resize);
    io_uring_queue_exit(&ring);
    return rc < 0 ? 1 : 0;
}
" CONFLUX_RUNTIME_HAS_IO_URING_RESIZE_RINGS)
else()
    set(CONFLUX_RUNTIME_HAS_IO_URING_RESIZE_RINGS FALSE)
endif()
if(CONFLUX_HAVE_IO_URING_CLONE_BUFFERS_OFFSET)
    check_cxx_source_runs("
#include <liburing.h>
#include <sys/uio.h>
int main() {
    io_uring src{};
    io_uring dst{};
    char storage[4096]{};
    iovec vec{storage, sizeof(storage)};
    if (io_uring_queue_init(8, &src, 0) < 0) {
        return 1;
    }
    if (io_uring_queue_init(8, &dst, 0) < 0) {
        io_uring_queue_exit(&src);
        return 1;
    }
    if (io_uring_register_buffers(&src, &vec, 1) < 0) {
        io_uring_queue_exit(&dst);
        io_uring_queue_exit(&src);
        return 1;
    }
    int rc = io_uring_clone_buffers_offset(&dst, &src, 0, 0, 1, 0);
    io_uring_unregister_buffers(&src);
    io_uring_queue_exit(&dst);
    io_uring_queue_exit(&src);
    return rc < 0 ? 1 : 0;
}
" CONFLUX_RUNTIME_HAS_IO_URING_CLONE_BUFFERS)
else()
    set(CONFLUX_RUNTIME_HAS_IO_URING_CLONE_BUFFERS FALSE)
endif()
unset(CMAKE_REQUIRED_LIBRARIES)
target_compile_definitions(conflux_uring
    PUBLIC CONFLUX_HAVE_IO_URING_RESIZE_RINGS=$<BOOL:${CONFLUX_HAVE_IO_URING_RESIZE_RINGS}>
    PUBLIC CONFLUX_RUNTIME_HAS_IOU_PBUF_RING_INC=$<BOOL:${CONFLUX_RUNTIME_HAS_IOU_PBUF_RING_INC}>
    PUBLIC CONFLUX_RUNTIME_HAS_IO_URING_RESIZE_RINGS=$<BOOL:${CONFLUX_RUNTIME_HAS_IO_URING_RESIZE_RINGS}>
    PUBLIC CONFLUX_RUNTIME_HAS_IO_URING_CLONE_BUFFERS=$<BOOL:${CONFLUX_RUNTIME_HAS_IO_URING_CLONE_BUFFERS}>
)

add_library(conflux_work STATIC)
target_sources(conflux_work
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/work/carrier_flags.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_model_a.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_scope.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_deadline.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_coro.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_streams.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_timer.cxx
        ${CONFLUX_SRC_ROOT}/work/root.cxx
        ${CONFLUX_SRC_ROOT}/work.cppm
        ${CONFLUX_SRC_ROOT}/work_api.cxx
)
target_sources(conflux_work
    PRIVATE
        ${CONFLUX_SRC_ROOT}/work_impl.cxx
)
target_compile_definitions(conflux_work
    PUBLIC CONFLUX_WORK_CARRIER_MODEL_A=$<BOOL:${CONFLUX_WORK_CARRIER_MODEL_A}>
    PUBLIC CONFLUX_WORK_CARRIER_MODEL_B=$<BOOL:${CONFLUX_WORK_CARRIER_MODEL_B}>
    PUBLIC CONFLUX_WORK_CORO_FRAME_POOL=$<BOOL:${CONFLUX_WORK_CORO_FRAME_POOL}>
    PUBLIC CONFLUX_WORK_ALLOC_STATS=$<BOOL:${CONFLUX_WORK_ALLOC_STATS}>
    PUBLIC CONFLUX_WORK_QUEUE_STATS=$<BOOL:${CONFLUX_WORK_QUEUE_STATS}>
)
target_link_libraries(conflux_work
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  PkgConfig::LIBURING
)

endif() # CONFLUX_NEEDS_RUNTIME (uring + work)

if(CONFLUX_WANT_PROCESS)
add_library(conflux_process STATIC)
target_sources(conflux_process
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/process.cxx
)
target_link_libraries(conflux_process
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
)
endif() # CONFLUX_WANT_PROCESS

if(CONFLUX_NEEDS_RUNTIME)
add_library(conflux_net_io_buffer STATIC)
target_sources(conflux_net_io_buffer
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/io_buffer.cxx
)
target_link_libraries(conflux_net_io_buffer
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
)
endif() # CONFLUX_NEEDS_RUNTIME (net io buffer)

if(CONFLUX_WANT_FILE_IO)
add_library(conflux_file_io STATIC)
target_sources(conflux_file_io
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/file_io/buffers.cxx
        ${CONFLUX_SRC_ROOT}/file_io/pipe_pool.cxx
        ${CONFLUX_SRC_ROOT}/file_io/reader.cxx
        ${CONFLUX_SRC_ROOT}/file_io/iopoll.cxx
        ${CONFLUX_SRC_ROOT}/file_io/driver.cxx
        ${CONFLUX_SRC_ROOT}/file_io/file_io.cxx
)
target_link_libraries(conflux_file_io
    PRIVATE conflux_options
    PUBLIC  conflux_file_io_sync
    PUBLIC  conflux_file_map
    PUBLIC  conflux_uring
    PUBLIC  conflux_uring_timeout
    PUBLIC  conflux_work
)

endif() # CONFLUX_WANT_FILE_IO

if(CONFLUX_WANT_FILE_WATCH)
add_library(conflux_file_watch STATIC)
target_sources(conflux_file_watch
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/file_watch.cxx
)
target_link_libraries(conflux_file_watch
    PRIVATE conflux_options
    PUBLIC  conflux_file_io
)
endif() # CONFLUX_WANT_FILE_WATCH

if(CONFLUX_WANT_TEMPLATES)
add_library(conflux_template STATIC)
target_sources(conflux_template
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/template.cxx
)
target_link_libraries(conflux_template
    PRIVATE conflux_options
    PUBLIC  conflux_json
    PUBLIC  conflux_utils
    PUBLIC  conflux_file_io_sync
)
endif() # CONFLUX_WANT_TEMPLATES

if(CONFLUX_WANT_TEMPLATES_WATCH)
add_library(conflux_template_watch STATIC)
target_sources(conflux_template_watch
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/template_watch.cxx
)
target_link_libraries(conflux_template_watch
    PRIVATE conflux_options
    PUBLIC  conflux_template
    PUBLIC  conflux_file_watch
)
endif() # CONFLUX_WANT_TEMPLATES_WATCH

if(CONFLUX_WANT_SOCKET_IO)
add_library(conflux_socket_io STATIC)
target_sources(conflux_socket_io
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/socket_io/socket_io.cxx
        ${CONFLUX_SRC_ROOT}/socket_io/socket_io_coro.cxx
        ${CONFLUX_SRC_ROOT}/socket_io/socket_io_blocking.cxx
)
target_sources(conflux_socket_io
    PRIVATE
        ${CONFLUX_SRC_ROOT}/socket_io/socket_io_coro_impl.cxx
)

target_link_libraries(conflux_socket_io
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_uring
    PUBLIC  conflux_work
)

endif() # CONFLUX_WANT_SOCKET_IO

if(CONFLUX_NEEDS_RUNTIME)
add_library(conflux_net_cancel STATIC)
target_sources(conflux_net_cancel
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/cancel.cxx
)
target_sources(conflux_net_cancel
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/cancel_impl.cxx
)
target_link_libraries(conflux_net_cancel
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
)
endif() # CONFLUX_NEEDS_RUNTIME (net cancel)

if(CONFLUX_WANT_DNS)
add_library(conflux_dns STATIC)
target_sources(conflux_dns
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/net/dns/dns.cxx
)
target_sources(conflux_dns
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/dns/dns_impl.cxx
)
target_link_libraries(conflux_dns
    PRIVATE conflux_options
    PUBLIC  conflux_work
    PRIVATE conflux_utils
    PUBLIC  conflux_file_io
    PUBLIC  conflux_socket_io
)

endif() # CONFLUX_WANT_DNS

if(CONFLUX_HAS_DB STREQUAL "true" AND CONFLUX_WANT_DB_POSTGRES)
    add_library(conflux_pg STATIC)
    target_sources(conflux_pg
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CONFLUX_SRC_ROOT}"
            FILES
            ${CONFLUX_SRC_ROOT}/db/types.cxx
            ${CONFLUX_SRC_ROOT}/db/params.cxx
            ${CONFLUX_SRC_ROOT}/db/result.cxx
            ${CONFLUX_SRC_ROOT}/db/connection.cxx
            ${CONFLUX_SRC_ROOT}/db/pool.cxx
            ${CONFLUX_SRC_ROOT}/db/pg.cxx
    )
    target_link_libraries(conflux_pg
        PRIVATE conflux_options
        PUBLIC  conflux_work
        PUBLIC  conflux_file_io
        PUBLIC  PkgConfig::LIBPQ
    )
    target_compile_definitions(conflux_pg PUBLIC CONFLUX_HAS_DB=1)

    add_library(conflux_db STATIC)
    target_sources(conflux_db
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CONFLUX_SRC_ROOT}"
            FILES
            ${CONFLUX_SRC_ROOT}/db/db.cxx
    )
    target_link_libraries(conflux_db
        PRIVATE conflux_options
        PUBLIC  conflux_pg
    )
    target_compile_definitions(conflux_db PUBLIC CONFLUX_HAS_DB=1)
endif()

# ---------------------------------------------------------------------------
# SIMD detection (before monolith, needed by conflux_json too)
# ---------------------------------------------------------------------------

if(NOT CONFLUX_USE_STDSIMD MATCHES "^(AUTO|STD26|STDX|ON|OFF)$")
    message(FATAL_ERROR
        "conflux: CONFLUX_USE_STDSIMD must be AUTO, STD26, STDX, ON, or OFF "
        "(got '${CONFLUX_USE_STDSIMD}')")
endif()

set(_conflux_simd_backend OFF)
if(CONFLUX_USE_STDSIMD STREQUAL "AUTO" OR CONFLUX_USE_STDSIMD STREQUAL "STD26")
    include(CheckCXXSourceCompiles)
    set(_STDSIMD_STD26_TEST_CODE "
#include <simd>
#include <cstddef>
#include <span>
using vec_t = std::simd::vec<signed char>;
using uvec_t = std::simd::vec<unsigned char>;
int main() {
    vec_t a{};
    vec_t b{};
    auto mask = (a == b) | (a < b);
    if (std::simd::any_of(mask))
        return static_cast<int>(std::simd::reduce_min_index(mask));
    unsigned char bytes[uvec_t::size] = {};
    auto u = std::simd::unchecked_load<uvec_t>(std::span<unsigned char>(bytes, uvec_t::size));
    u = std::simd::select(u == uvec_t{}, u | uvec_t(static_cast<unsigned char>(1)), u);
    std::simd::unchecked_store(u, std::span<unsigned char>(bytes, uvec_t::size));
    return static_cast<int>(vec_t::size);
}")
    set(_conflux_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
    check_cxx_source_compiles("${_STDSIMD_STD26_TEST_CODE}" CONFLUX_HAS_SIMD_STD26)
    set(CMAKE_CXX_STANDARD "${_conflux_saved_cxx_standard}")
    unset(_conflux_saved_cxx_standard)
    unset(CMAKE_REQUIRED_FLAGS)
    if(CONFLUX_HAS_SIMD_STD26)
        set(_conflux_simd_backend STD26)
    elseif(CONFLUX_USE_STDSIMD STREQUAL "STD26")
        message(FATAL_ERROR "conflux: CONFLUX_USE_STDSIMD=STD26 requires C++26 <simd> support")
    endif()
endif()

if((_conflux_simd_backend STREQUAL "OFF")
        AND (CONFLUX_USE_STDSIMD STREQUAL "AUTO"
            OR CONFLUX_USE_STDSIMD STREQUAL "STDX"
            OR CONFLUX_USE_STDSIMD STREQUAL "ON"))
    include(CheckCXXSourceCompiles)
    set(_STDSIMD_STDX_TEST_CODE "
#include <experimental/simd>
#include <cstddef>
namespace stdx = std::experimental::parallelism_v2;
using vec_t = stdx::native_simd<signed char>;
using uvec_t = stdx::native_simd<unsigned char>;
int main() {
    vec_t a{};
    vec_t b{};
    auto mask = (a == b) | (a < b);
    if (stdx::any_of(mask))
        return static_cast<int>(stdx::find_first_set(mask));
    unsigned char bytes[uvec_t::size()] = {};
    uvec_t u(bytes, stdx::element_aligned);
    stdx::where(u == uvec_t{}, u) = u | uvec_t(static_cast<unsigned char>(1));
    u.copy_to(bytes, stdx::element_aligned);
    return static_cast<int>(vec_t::size());
}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
    check_cxx_source_compiles("${_STDSIMD_STDX_TEST_CODE}" CONFLUX_HAS_SIMD_STDX)
    unset(CMAKE_REQUIRED_FLAGS)
    if(CONFLUX_HAS_SIMD_STDX)
        set(_conflux_simd_backend STDX)
    elseif(CONFLUX_USE_STDSIMD STREQUAL "STDX" OR CONFLUX_USE_STDSIMD STREQUAL "ON")
        message(FATAL_ERROR "conflux: CONFLUX_USE_STDSIMD=${CONFLUX_USE_STDSIMD} requires <experimental/simd> support")
    endif()
endif()

if(_conflux_simd_backend STREQUAL "STD26")
    add_library(conflux_simd_std26 OBJECT ${CONFLUX_SRC_ROOT}/simd_std26.cxx)
    target_compile_features(conflux_simd_std26 PRIVATE cxx_std_26)
    target_link_libraries(conflux_simd_std26 PRIVATE conflux_options)
    set(_conflux_simd_std26_options "-mavx2")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16
            AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
        list(APPEND _conflux_simd_std26_options "-fno-lto")
    endif()
    set_source_files_properties(${CONFLUX_SRC_ROOT}/simd_std26.cxx PROPERTIES
        COMPILE_OPTIONS "${_conflux_simd_std26_options}"
    )
    unset(_conflux_simd_std26_options)
    add_library(conflux_json_simd_std26 OBJECT ${CONFLUX_SRC_ROOT}/json_simd_std26.cxx)
    target_compile_features(conflux_json_simd_std26 PRIVATE cxx_std_26)
    target_link_libraries(conflux_json_simd_std26 PRIVATE conflux_options)
    set(_conflux_json_simd_std26_options "-mavx2")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16
            AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
        list(APPEND _conflux_json_simd_std26_options "-fno-lto")
    endif()
    set_source_files_properties(${CONFLUX_SRC_ROOT}/json_simd_std26.cxx PROPERTIES
        COMPILE_OPTIONS "${_conflux_json_simd_std26_options}"
    )
    unset(_conflux_json_simd_std26_options)
endif()

if(_conflux_simd_backend STREQUAL "STDX")
    add_library(conflux_simd_stdx OBJECT ${CONFLUX_SRC_ROOT}/simd_stdx.cxx)
    target_compile_features(conflux_simd_stdx PRIVATE cxx_std_23)
    target_link_libraries(conflux_simd_stdx PRIVATE conflux_options)
    set_source_files_properties(${CONFLUX_SRC_ROOT}/simd_stdx.cxx PROPERTIES
        COMPILE_OPTIONS "-mavx2"
    )
    add_library(conflux_json_simd_stdx OBJECT ${CONFLUX_SRC_ROOT}/json_simd_stdx.cxx)
    target_compile_features(conflux_json_simd_stdx PRIVATE cxx_std_23)
    target_link_libraries(conflux_json_simd_stdx PRIVATE conflux_options)
    set_source_files_properties(${CONFLUX_SRC_ROOT}/json_simd_stdx.cxx PROPERTIES
        COMPILE_OPTIONS "-mavx2"
    )
endif()

if(NOT _conflux_simd_backend STREQUAL "OFF")
    string(TOLOWER "${_conflux_simd_backend}" _conflux_simd_target_suffix)
    add_library(conflux_simd_runtime STATIC $<TARGET_OBJECTS:conflux_simd_${_conflux_simd_target_suffix}>)
    target_link_libraries(conflux_simd_runtime PRIVATE conflux_options)
endif()

if(NOT _conflux_simd_backend STREQUAL "OFF" AND TARGET conflux_crypto)
    target_compile_definitions(conflux_crypto PRIVATE CONFLUX_STDSIMD=1)
    target_link_libraries(conflux_crypto PRIVATE conflux_simd_runtime)
endif()

if(NOT _conflux_simd_backend STREQUAL "OFF" AND TARGET conflux_json)
    message(STATUS "conflux: ${_conflux_simd_backend} SIMD path enabled (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
    target_compile_definitions(conflux_json PRIVATE CONFLUX_JSON_USE_STDSIMD=1)
    string(TOLOWER "${_conflux_simd_backend}" _conflux_json_simd_target_suffix)
    target_sources(conflux_json PRIVATE $<TARGET_OBJECTS:conflux_json_simd_${_conflux_json_simd_target_suffix}>)
endif()

add_library(conflux_utils STATIC)
target_sources(conflux_utils
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/utils.cxx
)
target_link_libraries(conflux_utils
    PRIVATE conflux_options
    PUBLIC  conflux_types
)
if(NOT _conflux_simd_backend STREQUAL "OFF")
    target_compile_definitions(conflux_utils PRIVATE CONFLUX_STDSIMD=1)
    target_link_libraries(conflux_utils PRIVATE conflux_simd_runtime)
endif()

add_library(conflux_net_config STATIC)
target_sources(conflux_net_config
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/config.cxx
)
target_link_libraries(conflux_net_config
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_utils
)
if(CONFLUX_HAS_TLS STREQUAL "true")
    target_compile_definitions(conflux_net_config PUBLIC CONFLUX_HAS_TLS=1)
else()
    target_compile_definitions(conflux_net_config PUBLIC CONFLUX_HAS_TLS=0)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    target_compile_definitions(conflux_net_config PUBLIC CONFLUX_HAS_HTTP3=1)
else()
    target_compile_definitions(conflux_net_config PUBLIC CONFLUX_HAS_HTTP3=0)
endif()

# Keep the package-facing core anchor aligned with the documented
# "types + utils" core surface while leaving HTTP configuration as a separate
# support component.
target_link_libraries(conflux_core INTERFACE conflux_utils)

if(CONFLUX_WANT_HTTP_CORE OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_parse_helpers STATIC)
target_sources(conflux_http_parse_helpers
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_parse_helpers.cxx
)
target_link_libraries(conflux_http_parse_helpers
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_utils
)
endif() # CONFLUX_WANT_HTTP_CORE || CONFLUX_WANT_HTTP_SERVER

if(CONFLUX_HAS_TLS STREQUAL "true"
        AND (CONFLUX_HTTP_ROUTER_STACK_REQUESTED OR CONFLUX_HTTP_CLIENT_STACK_REQUESTED
             OR CONFLUX_WANT_SMTP))
add_library(conflux_net_tls STATIC)
target_sources(conflux_net_tls
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/tls.cxx
)
target_link_libraries(conflux_net_tls
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_utils
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_socket_io
    PUBLIC  conflux_net_cancel
    PUBLIC  OpenSSL::SSL
    PUBLIC  OpenSSL::Crypto
)
target_compile_definitions(conflux_net_tls PUBLIC CONFLUX_HAS_TLS=1)
endif() # CONFLUX_HAS_TLS && HTTP/TLS consumer


if(CONFLUX_JSON_REFLECT AND TARGET conflux_json)
    add_library(conflux_json_reflect STATIC)
    target_compile_features(conflux_json_reflect PUBLIC cxx_std_26)
    target_sources(conflux_json_reflect
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CONFLUX_SRC_ROOT}"
            FILES ${CONFLUX_SRC_ROOT}/json_reflect.cxx
    )
    target_link_libraries(conflux_json_reflect
        PUBLIC  conflux_json conflux_types
        PRIVATE conflux_options
    )
    target_compile_options(conflux_json_reflect PUBLIC -freflection)

    add_library(conflux_json_reflect_provider STATIC)
    target_sources(conflux_json_reflect_provider
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CONFLUX_SRC_ROOT}"
            FILES ${CONFLUX_SRC_ROOT}/json_reflect_provider.cxx
    )
    target_link_libraries(conflux_json_reflect_provider
        PUBLIC  conflux_json_reflect conflux_json_native_provider conflux_json_boundary
        PRIVATE conflux_options
    )
    target_compile_options(conflux_json_reflect_provider PUBLIC -freflection)
    message(STATUS "conflux: JSON P2996 reflection codec enabled (requires GCC 16+ -freflection)")
endif()

add_library(conflux_direct_slot_pool STATIC)
target_sources(conflux_direct_slot_pool
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/net/direct_slot_pool.cxx
)
target_link_libraries(conflux_direct_slot_pool
    PRIVATE conflux_options
    PRIVATE conflux_types
    PRIVATE conflux_utils)

if(CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED)
add_library(conflux_http_realtime STATIC)
target_sources(conflux_http_realtime
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/realtime.cxx
)
target_link_libraries(conflux_http_realtime
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_crypto
    PUBLIC  conflux_utils
)
if(TARGET conflux_net_tls)
    target_compile_definitions(conflux_http_realtime PUBLIC CONFLUX_HAS_TLS=1)
    target_link_libraries(conflux_http_realtime PUBLIC conflux_net_tls)
else()
    target_compile_definitions(conflux_http_realtime PUBLIC CONFLUX_HAS_TLS=0)
endif()
if(NOT _conflux_simd_backend STREQUAL "OFF")
    target_compile_definitions(conflux_http_realtime PRIVATE CONFLUX_STDSIMD=1)
    target_link_libraries(conflux_http_realtime PRIVATE conflux_simd_runtime)
endif()
if(CONFLUX_BUILD_FUZZ)
    target_compile_definitions(conflux_http_realtime PUBLIC CONFLUX_BUILD_FUZZ=1)
endif()
endif() # CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED

if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED)
add_library(conflux_http_response STATIC)
target_sources(conflux_http_response
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/response.cxx
)
target_link_libraries(conflux_http_response
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_file_map
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_utils
)
endif() # CONFLUX_HTTP_ROUTER_STACK_REQUESTED (HTTP response vocabulary)

if(TARGET conflux_http_response AND TARGET conflux_http_json)
add_library(conflux_http_response_json STATIC)
target_sources(conflux_http_response_json
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_response_json.cxx
)
target_link_libraries(conflux_http_response_json
    PRIVATE conflux_options
    PUBLIC  conflux_http_json
    PUBLIC  conflux_http_response
    PUBLIC  conflux_utils
)
endif() # conflux_http_response && conflux_http_json

if(TARGET conflux_http_response_json AND TARGET conflux_json_native_provider)
add_library(conflux_http_native_json STATIC)
target_sources(conflux_http_native_json
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_native_json.cxx
)
target_link_libraries(conflux_http_native_json
    PRIVATE conflux_options
    PUBLIC  conflux_http_response_json
    PUBLIC  conflux_json_native_provider
)
endif() # conflux_http_response_json && conflux_json_native_provider

if(CONFLUX_HTTP_STATIC_SURFACE_REQUESTED)
add_library(conflux_http_static_core STATIC)
target_sources(conflux_http_static_core
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/static_core.cxx
)
target_link_libraries(conflux_http_static_core
    PRIVATE conflux_options
    PUBLIC  conflux_types
)

add_library(conflux_http_static STATIC)
target_sources(conflux_http_static
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/static.cxx
)
target_link_libraries(conflux_http_static
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
    PUBLIC  conflux_net_config
)
endif() # CONFLUX_HTTP_STATIC_SURFACE_REQUESTED

if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED)
add_library(conflux_http_static_async STATIC)
target_sources(conflux_http_static_async
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/static_async_iface.cxx
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/static_async.cxx
)
target_link_libraries(conflux_http_static_async
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_file_map
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_response
    PUBLIC  conflux_http_static_core
    PUBLIC  conflux_utils
)

add_library(conflux_http_router STATIC)
target_sources(conflux_http_router
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/router.cxx
)
target_sources(conflux_http_router
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/router_impl.cxx
)
add_library(conflux_http_router_dispatch STATIC)
target_sources(conflux_http_router_dispatch
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/router_dispatch.cxx
)
target_link_libraries(conflux_http_router_dispatch
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_http_response
    PUBLIC  conflux_http_router_match
    PUBLIC  conflux_work
)
add_library(conflux_http_router_match STATIC)
target_sources(conflux_http_router_match
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/router_match.cxx
)
target_link_libraries(conflux_http_router_match
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_utils
)
add_library(conflux_http_router_static STATIC)
target_sources(conflux_http_router_static
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/router_static.cxx
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/router_static_impl.cxx
)
target_link_libraries(conflux_http_router_static
    PRIVATE conflux_options
    PRIVATE conflux_http_static_async
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_response
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_static_core
    PUBLIC  conflux_net_config
)
target_link_libraries(conflux_http_router
    PRIVATE conflux_options
    PRIVATE conflux_http_router_dispatch
    PRIVATE conflux_http_router_match
    PRIVATE conflux_http_router_static
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_response
    PRIVATE conflux_http_static_core
    PRIVATE conflux_http_static_async
    PUBLIC  conflux_work
    PUBLIC  conflux_utils
    PUBLIC  conflux_net_config
    PUBLIC  conflux_socket_io
)
endif() # HTTP router or router-dependent component

if(CONFLUX_WANT_HTTP_POLICY OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_policy STATIC)
target_sources(conflux_http_policy
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/net/cache_control.cxx
        ${CONFLUX_SRC_ROOT}/net/cors.cxx
        ${CONFLUX_SRC_ROOT}/net/etag.cxx
        ${CONFLUX_SRC_ROOT}/net/forwarded.cxx
        ${CONFLUX_SRC_ROOT}/net/ip_filter.cxx
        ${CONFLUX_SRC_ROOT}/net/rate_limit.cxx
        ${CONFLUX_SRC_ROOT}/net/redirect.cxx
        ${CONFLUX_SRC_ROOT}/net/response_cache.cxx
        ${CONFLUX_SRC_ROOT}/net/security.cxx
        ${CONFLUX_SRC_ROOT}/net/trailing_slash.cxx
)
target_link_libraries(conflux_http_policy
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
    PUBLIC  conflux_utils
)
endif() # CONFLUX_WANT_HTTP_POLICY || CONFLUX_WANT_HTTP_SERVER

if(CONFLUX_WANT_HTTP_AUTH OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_auth STATIC)
set(CONFLUX_HTTP_AUTH_SOURCES
    ${CONFLUX_SRC_ROOT}/net/auth.cxx
    ${CONFLUX_SRC_ROOT}/net/password_hash.cxx
    ${CONFLUX_SRC_ROOT}/net/cookie_signing.cxx
    ${CONFLUX_SRC_ROOT}/net/csrf.cxx
)
if(CONFLUX_HAS_TLS STREQUAL "true")
    list(APPEND CONFLUX_HTTP_AUTH_SOURCES ${CONFLUX_SRC_ROOT}/net/jwt.cxx)
endif()
target_sources(conflux_http_auth
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_HTTP_AUTH_SOURCES}
)
target_link_libraries(conflux_http_auth
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
    PUBLIC  conflux_crypto
    PUBLIC  conflux_utils
    PUBLIC  conflux_net_config
)
string(TOUPPER "${CONFLUX_PASSWORD_HASH_ARGON2_PROVIDER}" CONFLUX_ARGON2_PROVIDER_UPPER)
if(NOT CONFLUX_ARGON2_PROVIDER_UPPER MATCHES "^(AUTO|SYSTEM|RUNTIME|OFF)$")
    message(FATAL_ERROR
        "conflux: CONFLUX_PASSWORD_HASH_ARGON2_PROVIDER must be AUTO, SYSTEM, RUNTIME, or OFF "
        "(got '${CONFLUX_PASSWORD_HASH_ARGON2_PROVIDER}')")
endif()
if(CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "SYSTEM" AND NOT ARGON2_FOUND)
    message(FATAL_ERROR "conflux: Argon2 provider SYSTEM requires pkg-config module libargon2")
endif()
if((CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "AUTO" OR CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "SYSTEM")
    AND ARGON2_FOUND)
    target_link_libraries(conflux_http_auth PRIVATE PkgConfig::ARGON2)
    target_compile_definitions(conflux_http_auth PRIVATE CONFLUX_PASSWORD_HASH_ARGON2_LINKED=1)
    message(STATUS "conflux: password hashing Argon2 backend linked (${ARGON2_VERSION})")
elseif(NOT CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "OFF")
    target_compile_definitions(conflux_http_auth PRIVATE CONFLUX_PASSWORD_HASH_ARGON2_RUNTIME=1)
    if(CMAKE_DL_LIBS)
        target_link_libraries(conflux_http_auth PRIVATE ${CMAKE_DL_LIBS})
    endif()
    message(STATUS "conflux: password hashing Argon2 backend uses runtime loader")
else()
    message(STATUS "conflux: password hashing Argon2 backend disabled")
endif()
if(CONFLUX_HAS_TLS STREQUAL "true")
    target_compile_definitions(conflux_http_auth PUBLIC CONFLUX_HAS_TLS=1)
else()
    target_compile_definitions(conflux_http_auth PUBLIC CONFLUX_HAS_TLS=0)
endif()
endif() # CONFLUX_WANT_HTTP_AUTH || CONFLUX_WANT_HTTP_SERVER

if(CONFLUX_WANT_HTTP_OBSERVABILITY OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_observability STATIC)
set(CONFLUX_HTTP_OBSERVABILITY_SOURCES
    ${CONFLUX_SRC_ROOT}/net/request_id.cxx
    ${CONFLUX_SRC_ROOT}/net/structured_log.cxx
    ${CONFLUX_SRC_ROOT}/net/tracing.cxx
    ${CONFLUX_SRC_ROOT}/net/observability.cxx
)
if(CONFLUX_HAS_METRICS STREQUAL "true")
    list(APPEND CONFLUX_HTTP_OBSERVABILITY_SOURCES ${CONFLUX_SRC_ROOT}/net/metrics.cxx)
endif()
target_sources(conflux_http_observability
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_HTTP_OBSERVABILITY_SOURCES}
)
target_link_libraries(conflux_http_observability
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
    PUBLIC  conflux_utils
    PUBLIC  conflux_work
)
if(CONFLUX_HAS_METRICS STREQUAL "true")
    target_compile_definitions(conflux_http_observability PUBLIC CONFLUX_HAS_METRICS=1)
else()
    target_compile_definitions(conflux_http_observability PUBLIC CONFLUX_HAS_METRICS=0)
endif()
endif() # CONFLUX_WANT_HTTP_OBSERVABILITY || CONFLUX_WANT_HTTP_SERVER

if(CONFLUX_WANT_HTTP_OPENAPI OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_openapi STATIC)
target_sources(conflux_http_openapi
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/openapi.cxx
)
target_link_libraries(conflux_http_openapi
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
    PUBLIC  conflux_utils
)
endif() # CONFLUX_WANT_HTTP_OPENAPI || CONFLUX_WANT_HTTP_SERVER

if(CONFLUX_WANT_HTTP_VHOST OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_vhost STATIC)
target_sources(conflux_http_vhost
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/vhost.cxx
)
target_link_libraries(conflux_http_vhost
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
    PUBLIC  conflux_utils
    PUBLIC  conflux_work
)
endif() # CONFLUX_WANT_HTTP_VHOST || CONFLUX_WANT_HTTP_SERVER

if(CONFLUX_HTTP_CLIENT_STACK_REQUESTED)
add_library(conflux_dns_bridge STATIC)
target_sources(conflux_dns_bridge
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/client_dns_bridge.cxx
)
target_link_libraries(conflux_dns_bridge
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_dns
)

add_library(conflux_net_client STATIC)
target_sources(conflux_net_client
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
            ${CONFLUX_SRC_ROOT}/net/client_wire.cxx
            ${CONFLUX_SRC_ROOT}/net/client.cxx
            ${CONFLUX_SRC_ROOT}/net/http_client.cxx
)
target_link_libraries(conflux_net_client
    PRIVATE conflux_options
    PUBLIC  conflux_http_core
    PUBLIC  conflux_utils
    PUBLIC  conflux_work
    PUBLIC  conflux_dns_bridge
)
if(TARGET conflux_net_tls)
    target_compile_definitions(conflux_net_client PUBLIC CONFLUX_HAS_TLS=1)
    target_link_libraries(conflux_net_client PUBLIC conflux_net_tls)
else()
    target_compile_definitions(conflux_net_client PUBLIC CONFLUX_HAS_TLS=0)
endif()

add_library(conflux_net_async_client STATIC)
target_sources(conflux_net_async_client
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/client_async.cxx
)
target_sources(conflux_net_async_client
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/client_async_impl.cxx
)
target_link_libraries(conflux_net_async_client
    PRIVATE conflux_options
    PUBLIC  conflux_net_client
    PUBLIC  conflux_http_core
    PUBLIC  conflux_work
    PUBLIC  conflux_uring
    PUBLIC  conflux_socket_io
    PUBLIC  conflux_net_cancel
    PUBLIC  conflux_dns_bridge
)
if(TARGET conflux_net_tls)
    target_compile_definitions(conflux_net_async_client PUBLIC CONFLUX_HAS_TLS=1)
    target_link_libraries(conflux_net_async_client PUBLIC conflux_net_tls)
else()
    target_compile_definitions(conflux_net_async_client PUBLIC CONFLUX_HAS_TLS=0)
endif()
endif() # CONFLUX_HTTP_CLIENT_STACK_REQUESTED

if(CONFLUX_WANT_HTTP_COMPRESSION OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_compression STATIC)
set(CONFLUX_HTTP_COMPRESSION_SOURCES ${CONFLUX_SRC_ROOT}/net/compress.cxx)
if(CONFLUX_HAS_ZLIB STREQUAL "true")
    list(APPEND CONFLUX_HTTP_COMPRESSION_SOURCES ${CONFLUX_SRC_ROOT}/net/compress_backend_zlib.cxx)
endif()
if(CONFLUX_HAS_LIBDEFLATE STREQUAL "true")
    list(APPEND CONFLUX_HTTP_COMPRESSION_SOURCES ${CONFLUX_SRC_ROOT}/net/compress_backend_libdeflate.cxx)
endif()
if(CONFLUX_HAS_ZLIB_NG STREQUAL "true")
    list(APPEND CONFLUX_HTTP_COMPRESSION_SOURCES ${CONFLUX_SRC_ROOT}/net/compress_backend_zlibng.cxx)
endif()
if(CONFLUX_HAS_ISAL STREQUAL "true")
    list(APPEND CONFLUX_HTTP_COMPRESSION_SOURCES ${CONFLUX_SRC_ROOT}/net/compress_backend_isal.cxx)
endif()
target_sources(conflux_http_compression
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_HTTP_COMPRESSION_SOURCES}
)
target_link_libraries(conflux_http_compression
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
    PUBLIC  conflux_utils
)
target_compile_definitions(conflux_http_compression
    PUBLIC
        CONFLUX_HAS_COMPRESS=$<STREQUAL:${CONFLUX_HAS_COMPRESS},true>
        CONFLUX_HAS_ZLIB=$<STREQUAL:${CONFLUX_HAS_ZLIB},true>
        CONFLUX_HAS_LIBDEFLATE=$<STREQUAL:${CONFLUX_HAS_LIBDEFLATE},true>
        CONFLUX_HAS_ZLIB_NG=$<STREQUAL:${CONFLUX_HAS_ZLIB_NG},true>
        CONFLUX_HAS_ISAL=$<STREQUAL:${CONFLUX_HAS_ISAL},true>
        CONFLUX_HAS_BROTLI=$<STREQUAL:${CONFLUX_HAS_BROTLI},true>
        CONFLUX_HAS_ZSTD=$<STREQUAL:${CONFLUX_HAS_ZSTD},true>
)
if(CONFLUX_HAS_ZLIB STREQUAL "true")
    target_link_libraries(conflux_http_compression PUBLIC ZLIB::ZLIB)
endif()
if(CONFLUX_HAS_LIBDEFLATE STREQUAL "true")
    target_link_libraries(conflux_http_compression PUBLIC PkgConfig::LIBDEFLATE)
endif()
if(CONFLUX_HAS_ZLIB_NG STREQUAL "true")
    target_link_libraries(conflux_http_compression PUBLIC PkgConfig::ZLIB_NG)
endif()
if(CONFLUX_HAS_ISAL STREQUAL "true")
    target_link_libraries(conflux_http_compression PUBLIC PkgConfig::LIBISAL)
endif()
if(CONFLUX_HAS_BROTLI STREQUAL "true")
    target_link_libraries(conflux_http_compression PUBLIC PkgConfig::BROTLI)
endif()
if(CONFLUX_HAS_ZSTD STREQUAL "true")
    target_link_libraries(conflux_http_compression PUBLIC PkgConfig::ZSTD)
endif()
endif() # CONFLUX_WANT_HTTP_COMPRESSION || CONFLUX_WANT_HTTP_SERVER

if(CONFLUX_WANT_HTTP_PROXY OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_proxy STATIC)
target_sources(conflux_http_proxy
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/proxy.cxx
)
target_sources(conflux_http_proxy
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/proxy_impl.cxx
)
target_link_libraries(conflux_http_proxy
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_net_client
    PUBLIC  conflux_net_async_client
    PUBLIC  conflux_work
    PUBLIC  conflux_socket_io
    PUBLIC  conflux_utils
)
endif() # CONFLUX_WANT_HTTP_PROXY || CONFLUX_WANT_HTTP_SERVER

if(CONFLUX_WANT_SMTP OR (CONFLUX_WANT_HTTP_SERVER AND CONFLUX_HAS_TLS STREQUAL "true"))
add_library(conflux_net_smtp STATIC)
target_sources(conflux_net_smtp
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/smtp.cxx
)
target_link_libraries(conflux_net_smtp
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_crypto
    PUBLIC  conflux_utils
    PUBLIC  conflux_dns
    PUBLIC  conflux_work
    PUBLIC  conflux_net_tls
)
endif() # CONFLUX_WANT_SMTP || TLS HTTP server

if(CONFLUX_WANT_HTTP_CORE OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http1 STATIC)
target_sources(conflux_http1
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http1_parser.cxx
)
target_link_libraries(conflux_http1
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_net_config
)
endif() # CONFLUX_WANT_HTTP_CORE || CONFLUX_WANT_HTTP_SERVER (HTTP/1 parser)

if(CONFLUX_WANT_HTTP_SERVER AND CONFLUX_HAS_HTTP2 STREQUAL "true")
add_library(conflux_http2 STATIC)
target_sources(conflux_http2
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http2.cxx
)
target_link_libraries(conflux_http2
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  OpenSSL::SSL
    PUBLIC  OpenSSL::Crypto
    PUBLIC  PkgConfig::NGHTTP2
)
target_compile_definitions(conflux_http2 PUBLIC CONFLUX_HAS_HTTP2=1)
endif() # CONFLUX_WANT_HTTP_SERVER && HTTP/2 available

if(CONFLUX_WANT_HTTP_SERVER AND CONFLUX_HAS_HTTP3 STREQUAL "true")
add_library(conflux_http3 STATIC)
target_sources(conflux_http3
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http3.cxx
)
target_link_libraries(conflux_http3
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_net_config
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_router
    PUBLIC  OpenSSL::SSL
    PUBLIC  OpenSSL::Crypto
    PUBLIC  PkgConfig::NGTCP2
    PUBLIC  PkgConfig::NGTCP2_CRYPTO_OSSL
    PUBLIC  PkgConfig::NGHTTP3
)
target_compile_definitions(conflux_http3 PUBLIC CONFLUX_HAS_HTTP3=1)
endif() # CONFLUX_WANT_HTTP_SERVER && HTTP/3 available

if(CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_protocol STATIC)
target_sources(conflux_http_protocol
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_protocol.cxx
)
target_link_libraries(conflux_http_protocol
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_http1
)
if(TARGET conflux_http2)
    target_link_libraries(conflux_http_protocol PUBLIC conflux_http2)
    target_compile_definitions(conflux_http_protocol PUBLIC CONFLUX_HAS_HTTP2=1)
else()
    target_compile_definitions(conflux_http_protocol PUBLIC CONFLUX_HAS_HTTP2=0)
endif()
if(TARGET conflux_http3)
    target_link_libraries(conflux_http_protocol PUBLIC conflux_http3)
    target_compile_definitions(conflux_http_protocol PUBLIC CONFLUX_HAS_HTTP3=1)
else()
    target_compile_definitions(conflux_http_protocol PUBLIC CONFLUX_HAS_HTTP3=0)
endif()
endif() # CONFLUX_WANT_HTTP_SERVER (HTTP protocol umbrella)

if(CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_server_helpers STATIC)
target_sources(conflux_http_server_helpers
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_server_helpers.cxx
)
target_link_libraries(conflux_http_server_helpers
    PRIVATE conflux_options
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_response
    PUBLIC  conflux_utils
    PUBLIC  conflux_http_parse_helpers
)

add_library(conflux_http_server_config STATIC)
target_sources(conflux_http_server_config
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_server_config.cxx
)
target_link_libraries(conflux_http_server_config
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_uring
    PUBLIC  conflux_net_config
)

add_library(conflux_http_server STATIC)
target_sources(conflux_http_server
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_server.cxx
)
target_sources(conflux_http_server
    PRIVATE FILE_SET private_http_server_partitions TYPE CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
            ${CONFLUX_SRC_ROOT}/net/http_server_state.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_h2.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_tls.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_send.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_ws.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_recv.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_cqe.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_diag.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_loop.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_dispatch.cxx
)
target_sources(conflux_http_server
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/http_server_impl.cxx
)
target_link_libraries(conflux_http_server
    PRIVATE conflux_options
    PRIVATE conflux_http_server_helpers
    PRIVATE conflux_http_server_config
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_router
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_http_vhost
    PUBLIC  conflux_http_protocol
    PUBLIC  conflux_net_config
    PUBLIC  conflux_file_map
    PRIVATE conflux_direct_slot_pool
    PUBLIC  conflux_uring
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_socket_io
    PUBLIC  conflux_utils
)
if(TARGET conflux_net_tls)
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_TLS=1)
    target_link_libraries(conflux_http_server PUBLIC conflux_net_tls)
else()
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_TLS=0)
endif()
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_HTTP2=1)
else()
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_HTTP2=0)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_HTTP3=1)
else()
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_HTTP3=0)
endif()
if(CONFLUX_BUILD_FUZZ)
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_BUILD_FUZZ=1)
endif()
endif() # CONFLUX_WANT_HTTP_SERVER (server loop)

if(CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_app STATIC)
target_sources(conflux_http_app
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
            ${CONFLUX_SRC_ROOT}/net/app_defer.cxx
            ${CONFLUX_SRC_ROOT}/net/app_extractor_helpers.cxx
            ${CONFLUX_SRC_ROOT}/net/app_json_helpers.cxx
            ${CONFLUX_SRC_ROOT}/net/app_metadata_helpers.cxx
            ${CONFLUX_SRC_ROOT}/net/app_openapi.cxx
            ${CONFLUX_SRC_ROOT}/net/app_policies.cxx
            ${CONFLUX_SRC_ROOT}/net/app_response.cxx
            ${CONFLUX_SRC_ROOT}/net/app_route_helpers.cxx
            ${CONFLUX_SRC_ROOT}/net/app_traits.cxx
            ${CONFLUX_SRC_ROOT}/net/app_types.cxx
            ${CONFLUX_SRC_ROOT}/net/app.cxx
)
target_link_libraries(conflux_http_app
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_net_config
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_response
    PUBLIC  conflux_json
    PUBLIC  conflux_json_boundary
    PUBLIC  conflux_http_router
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_http_server
    PUBLIC  conflux_work
)
if(TARGET conflux_http_native_json)
    target_link_libraries(conflux_http_app PUBLIC conflux_http_native_json)
    target_compile_definitions(conflux_http_app PUBLIC CONFLUX_HAS_JSON=1)
else()
    target_compile_definitions(conflux_http_app PUBLIC CONFLUX_HAS_JSON=0)
endif()
target_link_libraries(conflux_http_app
    PUBLIC  conflux_http_json
    PUBLIC  conflux_http_response_json
    PUBLIC  conflux_net_client
    PUBLIC  conflux_http_protocol
    PUBLIC  conflux_http_auth
    PUBLIC  conflux_http_compression
    PUBLIC  conflux_http_policy
    PUBLIC  conflux_http_observability
    PUBLIC  conflux_http_openapi
    PUBLIC  conflux_http_vhost
)
if(TARGET conflux_http2)
    target_link_libraries(conflux_http_app PUBLIC conflux_http2)
endif()
if(TARGET conflux_http3)
    target_link_libraries(conflux_http_app PUBLIC conflux_http3)
endif()
endif() # CONFLUX_WANT_HTTP_SERVER (app facade)

if(TARGET conflux_http_response_json AND TARGET conflux_http_app AND TARGET conflux_http_router)
target_sources(conflux_http_app
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_app_json.cxx
)
add_library(conflux_http_app_json INTERFACE)
target_link_libraries(conflux_http_app_json
    INTERFACE conflux_http_app
    INTERFACE conflux_http_router
    INTERFACE conflux_http_response_json
)
endif() # conflux_http_response_json && conflux_http_app && conflux_http_router

if(CONFLUX_WANT_HTTP_SERVER)
target_sources(conflux_http_app
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
            ${CONFLUX_SRC_ROOT}/net/http_facade_problem.cxx
            ${CONFLUX_SRC_ROOT}/net/http_facade.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_umbrella.cxx
            ${CONFLUX_SRC_ROOT}/net/http.cppm
)
add_library(conflux_net_http INTERFACE)
target_link_libraries(conflux_net_http
    INTERFACE conflux_types
    INTERFACE conflux_net_config
    INTERFACE conflux_http_core
    INTERFACE conflux_http_json
    INTERFACE conflux_http_response_json
    INTERFACE conflux_http_app_json
    INTERFACE conflux_http_router
    INTERFACE conflux_net_client
    INTERFACE conflux_http_protocol
    INTERFACE conflux_http_server
    INTERFACE conflux_http_app
    INTERFACE conflux_http_auth
    INTERFACE conflux_http_compression
    INTERFACE conflux_http_policy
    INTERFACE conflux_http_observability
    INTERFACE conflux_http_openapi
    INTERFACE conflux_http_vhost
)
if(TARGET conflux_http_native_json)
    target_link_libraries(conflux_net_http INTERFACE conflux_http_native_json)
endif()
if(TARGET conflux_json)
    target_link_libraries(conflux_net_http INTERFACE conflux_json)
endif()
if(CONFLUX_HAS_TLS STREQUAL "true")
    target_compile_definitions(conflux_net_http INTERFACE CONFLUX_HAS_TLS=1)
else()
    target_compile_definitions(conflux_net_http INTERFACE CONFLUX_HAS_TLS=0)
endif()
if(CONFLUX_HAS_METRICS STREQUAL "true")
    target_compile_definitions(conflux_net_http INTERFACE CONFLUX_HAS_METRICS=1)
else()
    target_compile_definitions(conflux_net_http INTERFACE CONFLUX_HAS_METRICS=0)
endif()
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    target_compile_definitions(conflux_net_http INTERFACE CONFLUX_HAS_HTTP2=1)
else()
    target_compile_definitions(conflux_net_http INTERFACE CONFLUX_HAS_HTTP2=0)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    target_compile_definitions(conflux_net_http INTERFACE CONFLUX_HAS_HTTP3=1)
else()
    target_compile_definitions(conflux_net_http INTERFACE CONFLUX_HAS_HTTP3=0)
endif()
endif() # CONFLUX_WANT_HTTP_SERVER (HTTP umbrella)

# ---------------------------------------------------------------------------
# Aggregate target (legacy convenience import surface)
# ---------------------------------------------------------------------------

if(CONFLUX_WANT_HTTP_SERVER)

add_library(conflux STATIC)

target_sources(conflux
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/conflux.cppm
)

target_sources(conflux
    PUBLIC FILE_SET generated_mods TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
        FILES
        "${CMAKE_CURRENT_BINARY_DIR}/src/conflux_features.cxx"
)

# ---------------------------------------------------------------------------
# Link dependencies
# ---------------------------------------------------------------------------

target_link_libraries(conflux
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_utils
    PUBLIC  conflux_net_config
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_json
    PUBLIC  conflux_http_response_json
    PUBLIC  conflux_http_app_json
    PUBLIC  conflux_http_router
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_static_core
    PUBLIC  conflux_http_static_async
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_http_response
    PUBLIC  conflux_http_policy
    PUBLIC  conflux_http_auth
    PUBLIC  conflux_http_observability
    PUBLIC  conflux_http_openapi
    PUBLIC  conflux_http_vhost
    PUBLIC  conflux_http_protocol
    PUBLIC  conflux_http_server
    PUBLIC  conflux_http_app
    PUBLIC  conflux_net_http
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_socket_io
    PRIVATE conflux_direct_slot_pool
)
if(TARGET conflux_process)
    target_link_libraries(conflux PUBLIC conflux_process)
endif()
if(TARGET conflux_net_io_buffer)
    target_link_libraries(conflux PUBLIC conflux_net_io_buffer)
endif()
if(TARGET conflux_template)
    target_link_libraries(conflux PUBLIC conflux_template)
endif()
if(TARGET conflux_template_watch)
    target_link_libraries(conflux PUBLIC conflux_template_watch)
elseif(TARGET conflux_file_watch)
    target_link_libraries(conflux PUBLIC conflux_file_watch)
endif()
if(TARGET conflux_json_boundary)
    target_link_libraries(conflux PUBLIC conflux_json_boundary)
endif()
if(TARGET conflux_http_parse_helpers)
    target_link_libraries(conflux PUBLIC conflux_http_parse_helpers)
endif()
if(TARGET conflux_json)
    target_link_libraries(conflux PUBLIC conflux_json)
endif()
if(TARGET conflux_json_native_provider)
    target_link_libraries(conflux PUBLIC conflux_json_native_provider)
endif()
if(TARGET conflux_http_native_json)
    target_link_libraries(conflux PUBLIC conflux_http_native_json)
endif()
if(TARGET conflux_json_file)
    target_link_libraries(conflux PUBLIC conflux_json_file)
endif()
if(TARGET conflux_crypto)
    target_link_libraries(conflux PUBLIC conflux_crypto)
endif()
if(TARGET conflux_file_map)
    target_link_libraries(conflux PUBLIC conflux_file_map)
endif()
if(TARGET conflux_dns)
    target_link_libraries(conflux PUBLIC conflux_dns)
endif()
if(TARGET conflux_net_cancel)
    target_link_libraries(conflux PUBLIC conflux_net_cancel)
endif()
if(TARGET conflux_net_tls)
    target_link_libraries(conflux PUBLIC conflux_net_tls)
endif()
if(TARGET conflux_dns_bridge)
    target_link_libraries(conflux PUBLIC conflux_dns_bridge)
endif()
if(TARGET conflux_net_client)
    target_link_libraries(conflux PUBLIC conflux_net_client)
endif()
if(TARGET conflux_net_async_client)
    target_link_libraries(conflux PUBLIC conflux_net_async_client)
endif()
if(TARGET conflux_http_compression)
    target_link_libraries(conflux PUBLIC conflux_http_compression)
endif()
if(TARGET conflux_http_proxy)
    target_link_libraries(conflux PUBLIC conflux_http_proxy)
endif()
if(TARGET conflux_net_smtp)
    target_link_libraries(conflux PUBLIC conflux_net_smtp)
endif()

if(CONFLUX_JSON_AVX2)
    target_compile_options(conflux PRIVATE -mavx2 -mbmi2)
endif()

# Template engine / hot-reload surface. The file-watch module is now compiled
# by conflux_file_watch; the umbrella module only re-exports it when present.
target_compile_definitions(conflux
    PUBLIC
        CONFLUX_HAS_JSON=$<BOOL:${CONFLUX_WANT_JSON}>
        CONFLUX_HAS_JSON_FILE=$<BOOL:${CONFLUX_WANT_JSON_FILE}>
        CONFLUX_HAS_TEMPLATES=$<BOOL:${CONFLUX_WANT_TEMPLATES}>
        CONFLUX_HAS_TEMPLATES_WATCH=$<BOOL:${CONFLUX_WANT_TEMPLATES_WATCH}>
        CONFLUX_HAS_FILE_WATCH=$<BOOL:${CONFLUX_WANT_FILE_WATCH}>
)

# Feature macros and external dependencies now propagate through the split
# component targets (`conflux::net_tls`, `conflux::http_compression`,
# `conflux::http_protocol`, `conflux::db`, ...). Keep the aggregate target
# thin so optional packages are not pulled in just because the umbrella module
# is built.

# Fuzz harness: propagate the macro so CONFLUX_FUZZ_EXPORT expands to `export`
# in module interfaces, making fuzzable internal helpers reachable from
# fuzz drivers. Otherwise stays empty and LTO/inlining are free to optimize.
if(CONFLUX_BUILD_FUZZ)
    target_compile_definitions(conflux PUBLIC CONFLUX_BUILD_FUZZ=1)
endif()

if(CONFLUX_BUILD_EXAMPLES)
    set(CONFLUX_EXAMPLES_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/conflux_examples_manifest.txt")
    file(WRITE "${CONFLUX_EXAMPLES_MANIFEST}" "Conflux examples manifest\n\n")
    function(conflux_note_example TARGET_NAME STATUS REASON)
        file(APPEND "${CONFLUX_EXAMPLES_MANIFEST}" "${TARGET_NAME}: ${STATUS} - ${REASON}\n")
    endfunction()

    add_executable(conflux_quickstart_hello examples/quickstart/hello.cxx)
    target_link_libraries(conflux_quickstart_hello PRIVATE conflux conflux_options)

    add_executable(conflux_quickstart_middleware examples/quickstart/middleware.cxx)
    target_link_libraries(conflux_quickstart_middleware PRIVATE conflux conflux_options)

    add_executable(conflux_quickstart_sse examples/quickstart/sse.cxx)
    target_link_libraries(conflux_quickstart_sse PRIVATE conflux conflux_options)

    add_executable(conflux_quickstart_static_files examples/quickstart/static_files.cxx)
    target_link_libraries(conflux_quickstart_static_files PRIVATE conflux conflux_options)

    add_executable(conflux_quickstart_websocket examples/quickstart/websocket.cxx)
    target_link_libraries(conflux_quickstart_websocket PRIVATE conflux conflux_options)

    add_executable(conflux_quickstart_openapi examples/quickstart/openapi.cxx)
    target_link_libraries(conflux_quickstart_openapi PRIVATE conflux conflux_options)

    if(TARGET conflux_http_native_json AND TARGET conflux_json)
        add_executable(conflux_quickstart_json_crud examples/quickstart/json_crud.cxx)
        target_link_libraries(conflux_quickstart_json_crud PRIVATE conflux conflux_options)
    endif()

    if(CONFLUX_HAS_DB STREQUAL "true" AND TARGET conflux_db)
        add_executable(conflux_quickstart_postgres examples/quickstart/postgres_json.cxx)
        target_link_libraries(conflux_quickstart_postgres
            PRIVATE conflux conflux_pg conflux_file_io conflux_work conflux_options)
    endif()

    # Easy HTTP examples (first-contact / v1-facing)
    add_executable(conflux_hello examples/hello.cxx)
    target_link_libraries(conflux_hello PRIVATE conflux conflux_options)

    add_executable(conflux_middleware examples/middleware.cxx)
    target_link_libraries(conflux_middleware PRIVATE conflux conflux_options)

    add_executable(conflux_sse examples/sse.cxx)
    target_link_libraries(conflux_sse PRIVATE conflux conflux_options)

    add_executable(conflux_static examples/static.cxx)
    target_link_libraries(conflux_static PRIVATE conflux conflux_options)

    # Additional examples
    add_executable(conflux_forms examples/forms.cxx)
    target_link_libraries(conflux_forms PRIVATE conflux conflux_options)

    add_executable(conflux_gzip examples/gzip.cxx)
    target_link_libraries(conflux_gzip PRIVATE conflux conflux_options)

    add_executable(conflux_http_client examples/http_client.cxx)
    target_link_libraries(conflux_http_client PRIVATE conflux conflux_options)

    add_executable(conflux_dual examples/advanced/dual.cxx)
    target_link_libraries(conflux_dual PRIVATE conflux conflux_options)

    add_executable(conflux_process_run_example examples/advanced/process_run.cxx)
    target_link_libraries(conflux_process_run_example PRIVATE conflux conflux_options)

    add_executable(conflux_crypto_sealing_example examples/advanced/crypto_sealing.cxx)
    target_link_libraries(conflux_crypto_sealing_example PRIVATE conflux conflux_options)

    add_executable(conflux_http_observability_example examples/advanced/http_observability.cxx)
    target_link_libraries(conflux_http_observability_example PRIVATE conflux conflux_options)

    add_executable(conflux_http_lifecycle_example examples/advanced/http_lifecycle.cxx)
    target_link_libraries(conflux_http_lifecycle_example PRIVATE conflux conflux_options)

    add_executable(conflux_production_showcase_example examples/advanced/production_showcase.cxx)
    target_link_libraries(conflux_production_showcase_example PRIVATE conflux conflux_options)

    add_executable(conflux_http_policy_stack_example examples/advanced/http_policy_stack.cxx)
    target_link_libraries(conflux_http_policy_stack_example PRIVATE conflux conflux_options)

    add_executable(conflux_vhost_openapi_example examples/advanced/vhost_openapi.cxx)
    target_link_libraries(conflux_vhost_openapi_example PRIVATE conflux conflux_options)

    add_executable(conflux_http_client_builder_example examples/advanced/http_client_builder.cxx)
    target_link_libraries(conflux_http_client_builder_example PRIVATE conflux conflux_options)

    if(TARGET conflux_http_app_json AND TARGET conflux_http_native_json AND TARGET conflux_json)
        add_executable(conflux_api_typed_json_example examples/advanced/manual_json_members.cxx)
        target_link_libraries(conflux_api_typed_json_example
            PRIVATE conflux conflux_options)

        add_executable(conflux_http_explicit_offload_example examples/advanced/explicit_offload.cxx)
        target_link_libraries(conflux_http_explicit_offload_example
            PRIVATE conflux conflux_work conflux_options)
    endif()

    if(TARGET conflux_http_native_json AND TARGET conflux_json)
        add_executable(conflux_http_client_json_example examples/advanced/http_client_json.cxx)
        target_link_libraries(conflux_http_client_json_example
            PRIVATE conflux_http_native_json conflux_json conflux_options)

        add_executable(conflux_custom_json_provider_example examples/advanced/custom_json_provider.cxx)
        target_link_libraries(conflux_custom_json_provider_example
            PRIVATE conflux_http_native_json conflux_json conflux_options)
    endif()

    add_executable(conflux_work_join_all_example examples/advanced/work_join_all.cxx)
    target_link_libraries(conflux_work_join_all_example PRIVATE conflux conflux_options)

    if(CONFLUX_WANT_TEMPLATES AND TARGET conflux_template)
        add_executable(conflux_template_pages_example examples/advanced/template_pages.cxx)
        target_link_libraries(conflux_template_pages_example PRIVATE conflux conflux_template conflux_options)
    endif()

    if(CONFLUX_HAS_HTTP3 STREQUAL "true")
        add_executable(conflux_h3_probe examples/advanced/h3_probe.cxx)
        target_link_libraries(conflux_h3_probe PRIVATE conflux conflux_options)
        add_executable(conflux_h3_server examples/advanced/h3_server.cxx)
        target_link_libraries(conflux_h3_server PRIVATE conflux conflux_options)
    endif()

    if(CONFLUX_HAS_DB STREQUAL "true" AND TARGET conflux_db)
        add_executable(conflux_db_basic examples/advanced/db_basic.cxx)
        target_link_libraries(conflux_db_basic
            PRIVATE conflux_pg conflux_file_io conflux_work conflux_options)

        add_executable(conflux_db_pool examples/advanced/db_pool.cxx)
        target_link_libraries(conflux_db_pool
            PRIVATE conflux_pg conflux_file_io conflux_work conflux_options)

        add_executable(conflux_advanced_postgres examples/advanced/postgres.cxx)
        target_link_libraries(conflux_advanced_postgres
            PRIVATE conflux_pg conflux_file_io conflux_work conflux_options)
    endif()

    # JSON examples (use monolith for convenience)
    add_executable(conflux_json_example examples/advanced/json.cxx)
    target_link_libraries(conflux_json_example PRIVATE conflux conflux_options)

    add_executable(conflux_json_config_example examples/advanced/json_config.cxx)
    target_link_libraries(conflux_json_config_example PRIVATE conflux conflux_options)

    add_executable(conflux_json_stream_ingest_example examples/advanced/json_stream_ingest.cxx)
    target_link_libraries(conflux_json_stream_ingest_example PRIVATE conflux conflux_options)

    add_executable(conflux_json_diagnostics_example examples/advanced/json_diagnostics.cxx)
    target_link_libraries(conflux_json_diagnostics_example PRIVATE conflux conflux_options)

    add_executable(conflux_json_transform_example examples/advanced/json_transform.cxx)
    target_link_libraries(conflux_json_transform_example PRIVATE conflux conflux_options)
endif() # CONFLUX_BUILD_EXAMPLES

endif() # CONFLUX_WANT_HTTP_SERVER

# ---------------------------------------------------------------------------
# Tests, benchmarks, and fuzzing
# ---------------------------------------------------------------------------

if(CONFLUX_BUILD_TESTS OR (CONFLUX_BUILD_FUZZ AND CONFLUX_BUILD_FUZZ_SMOKE_TESTS))
    enable_testing()
endif()

if(CONFLUX_BUILD_TESTS)
    if(TARGET conflux_uring)
        target_compile_definitions(conflux_uring PRIVATE CONFLUX_TESTING)
    endif()
    add_subdirectory(tests)
endif()

if(CONFLUX_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(CONFLUX_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Public package targets / install-export
# ---------------------------------------------------------------------------

set(CONFLUX_PACKAGE_COMPONENTS)
set(CONFLUX_PACKAGE_TARGETS)
set(CONFLUX_PACKAGE_SUPPORT_COMPONENTS)
set(CONFLUX_PACKAGE_SUPPORT_TARGETS)
set(CONFLUX_PACKAGE_ALL_COMPONENTS)
set(CONFLUX_PACKAGE_ALL_TARGETS)

function(conflux_component target export_name)
    set(options)
    set(one_value_args KIND)
    set(multi_value_args)
    cmake_parse_arguments(CONFLUX_COMPONENT
        "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(NOT CONFLUX_COMPONENT_KIND)
        set(CONFLUX_COMPONENT_KIND REQUESTABLE)
    endif()
    if(NOT CONFLUX_COMPONENT_KIND MATCHES "^(REQUESTABLE|SUPPORT)$")
        message(FATAL_ERROR
            "conflux: component '${export_name}' has invalid KIND '${CONFLUX_COMPONENT_KIND}'")
    endif()

    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
        if(NOT TARGET conflux::${export_name})
            add_library(conflux::${export_name} ALIAS ${target})
        endif()

        set(_all_components ${CONFLUX_PACKAGE_ALL_COMPONENTS})
        list(APPEND _all_components "${export_name}")
        set(CONFLUX_PACKAGE_ALL_COMPONENTS "${_all_components}" PARENT_SCOPE)

        set(_all_targets ${CONFLUX_PACKAGE_ALL_TARGETS})
        list(APPEND _all_targets "conflux::${export_name}")
        set(CONFLUX_PACKAGE_ALL_TARGETS "${_all_targets}" PARENT_SCOPE)

        if(CONFLUX_COMPONENT_KIND STREQUAL "SUPPORT")
            set(_support_components ${CONFLUX_PACKAGE_SUPPORT_COMPONENTS})
            list(APPEND _support_components "${export_name}")
            set(CONFLUX_PACKAGE_SUPPORT_COMPONENTS "${_support_components}" PARENT_SCOPE)

            set(_support_targets ${CONFLUX_PACKAGE_SUPPORT_TARGETS})
            list(APPEND _support_targets "conflux::${export_name}")
            set(CONFLUX_PACKAGE_SUPPORT_TARGETS "${_support_targets}" PARENT_SCOPE)
        else()
            set(_components ${CONFLUX_PACKAGE_COMPONENTS})
            list(APPEND _components "${export_name}")
            set(CONFLUX_PACKAGE_COMPONENTS "${_components}" PARENT_SCOPE)

            set(_targets ${CONFLUX_PACKAGE_TARGETS})
            list(APPEND _targets "conflux::${export_name}")
            set(CONFLUX_PACKAGE_TARGETS "${_targets}" PARENT_SCOPE)
        endif()

    endif()
endfunction()

macro(conflux_public_component target export_name)
    conflux_component(${target} ${export_name} KIND REQUESTABLE)
endmacro()

conflux_public_component(conflux_core core)
conflux_public_component(conflux_types types)
conflux_public_component(conflux_utils utils)
conflux_public_component(conflux_net_config net_config)
conflux_public_component(conflux_net_cancel net_cancel)
conflux_public_component(conflux_net_tls net_tls)
conflux_public_component(conflux_process process)
conflux_public_component(conflux_net_io_buffer net_io_buffer)
conflux_public_component(conflux_crypto crypto)
conflux_public_component(conflux_json_boundary json_boundary)
conflux_public_component(conflux_json json)
conflux_public_component(conflux_json_native_provider json_native_provider)
conflux_public_component(conflux_json_file json_file)
conflux_public_component(conflux_json_reflect json_reflect)
conflux_public_component(conflux_json_reflect_provider json_reflect_provider)
conflux_public_component(conflux_template template)
conflux_public_component(conflux_file_watch file_watch)
conflux_public_component(conflux_template_watch template_watch)
conflux_public_component(conflux_http_parse_helpers http_parse_helpers)
conflux_public_component(conflux_http_core http_core)
conflux_public_component(conflux_http_router http_router)
conflux_public_component(conflux_http_router_dispatch router_dispatch)
conflux_public_component(conflux_http_router_match router_match)
conflux_public_component(conflux_http_router_static router_static)
conflux_public_component(conflux_http_static http_static)
conflux_public_component(conflux_http_static_core http_static_core)
conflux_public_component(conflux_http_static_async http_static_async)
conflux_public_component(conflux_http_realtime http_realtime)
conflux_public_component(conflux_http_response http_response)
conflux_public_component(conflux_http_server_helpers http_server_helpers)
conflux_public_component(conflux_http_server_config http_server_config)
conflux_public_component(conflux_http_policy http_policy)
conflux_public_component(conflux_http_auth http_auth)
conflux_public_component(conflux_http_compression http_compression)
conflux_public_component(conflux_net_client http_client)
conflux_public_component(conflux_net_async_client http_async_client)
conflux_public_component(conflux_http_proxy http_proxy)
conflux_public_component(conflux_net_smtp smtp)
conflux_public_component(conflux_dns_bridge dns_bridge)
conflux_public_component(conflux_http_observability http_observability)
conflux_public_component(conflux_http_openapi http_openapi)
conflux_public_component(conflux_http_vhost http_vhost)
conflux_public_component(conflux_http_json http_json)
conflux_public_component(conflux_http_response_json http_response_json)
conflux_public_component(conflux_http_app_json http_app_json)
conflux_public_component(conflux_http_native_json http_native_json)
conflux_public_component(conflux_http1 http1)
conflux_public_component(conflux_http2 http2)
conflux_public_component(conflux_http3 http3)
conflux_public_component(conflux_http_protocol http_protocol)
conflux_public_component(conflux_http_server http_server)
conflux_public_component(conflux_http_app http_app)
conflux_public_component(conflux_net_http http)
conflux_public_component(conflux_uring uring)
conflux_public_component(conflux_uring_timeout uring_timeout)
conflux_public_component(conflux_work runtime)
conflux_public_component(conflux_file_io_sync file_io_sync)
conflux_public_component(conflux_file_map file_map)
conflux_public_component(conflux_file_io file_io)
conflux_public_component(conflux_socket_io socket_io)
conflux_public_component(conflux_dns dns)
conflux_public_component(conflux_db db)
conflux_public_component(conflux_pg pg)
conflux_public_component(conflux umbrella)

# Internal-but-exported support targets needed by static-library link interfaces.
conflux_component(conflux_options _options KIND SUPPORT)
conflux_component(conflux_direct_slot_pool _direct_slot_pool KIND SUPPORT)
conflux_component(conflux_simd_runtime _simd_runtime KIND SUPPORT)

if(TARGET conflux AND NOT TARGET conflux::conflux)
    add_library(conflux::conflux ALIAS conflux)
    list(APPEND CONFLUX_PACKAGE_COMPONENTS conflux)
    list(APPEND CONFLUX_PACKAGE_TARGETS conflux::conflux)
    list(APPEND CONFLUX_PACKAGE_ALL_COMPONENTS conflux)
    list(APPEND CONFLUX_PACKAGE_ALL_TARGETS conflux::conflux)
endif()

set(CONFLUX_INSTALL_INTERFACE_TARGETS)
foreach(_target IN ITEMS
        conflux_options
        conflux_core
        conflux_simd_runtime)
    if(TARGET ${_target})
        list(APPEND CONFLUX_INSTALL_INTERFACE_TARGETS ${_target})
    endif()
endforeach()

set(CONFLUX_INSTALL_MODULE_TARGETS)
foreach(_target IN ITEMS
        conflux_types
        conflux_utils
        conflux_net_config
        conflux_net_cancel
        conflux_net_tls
        conflux_process
        conflux_net_io_buffer
        conflux_crypto
        conflux_json_boundary
        conflux_json
        conflux_json_native_provider
        conflux_json_file
        conflux_json_reflect
        conflux_json_reflect_provider
        conflux_template
        conflux_template_watch
        conflux_file_watch
        conflux_http_parse_helpers
        conflux_http_core
        conflux_http_router
        conflux_http_router_dispatch
        conflux_http_router_match
        conflux_http_router_static
        conflux_http_static
        conflux_http_static_core
        conflux_http_static_async
        conflux_http_realtime
        conflux_http_response
        conflux_http_server_helpers
        conflux_http_server_config
        conflux_http_policy
        conflux_http_auth
        conflux_http_compression
        conflux_net_client
        conflux_net_async_client
        conflux_http_proxy
        conflux_net_smtp
        conflux_dns_bridge
        conflux_http_observability
        conflux_http_openapi
        conflux_http_vhost
        conflux_http_json
        conflux_http_response_json
        conflux_http_app_json
        conflux_http_native_json
        conflux_http1
        conflux_http2
        conflux_http3
        conflux_http_protocol
        conflux_http_server
        conflux_http_app
        conflux_net_http
        conflux_uring
        conflux_uring_timeout
        conflux_work
        conflux_file_io_sync
        conflux_file_map
        conflux_file_io
        conflux_socket_io
        conflux_dns
        conflux_pg
        conflux_db
        conflux_direct_slot_pool)
    if(TARGET ${_target})
        list(APPEND CONFLUX_INSTALL_MODULE_TARGETS ${_target})
    endif()
endforeach()

foreach(_target IN LISTS CONFLUX_INSTALL_MODULE_TARGETS)
    if(NOT _target STREQUAL "conflux_types")
        get_target_property(_target_type ${_target} TYPE)
        if(_target_type STREQUAL "INTERFACE_LIBRARY")
            target_link_libraries(${_target} INTERFACE conflux_types)
        else()
            target_link_libraries(${_target} PUBLIC conflux_types)
        endif()
    endif()
endforeach()

if(CONFLUX_INSTALL_INTERFACE_TARGETS)
    foreach(_target IN LISTS CONFLUX_INSTALL_INTERFACE_TARGETS)
        get_target_property(_export_name ${_target} EXPORT_NAME)
        install(TARGETS ${_target}
            EXPORT confluxTargets-${_export_name}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endforeach()
endif()

if(CONFLUX_INSTALL_MODULE_TARGETS)
    foreach(_target IN LISTS CONFLUX_INSTALL_MODULE_TARGETS)
        get_target_property(_export_name ${_target} EXPORT_NAME)
        install(TARGETS ${_target}
            EXPORT confluxTargets-${_export_name}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules
            FILE_SET generated_mods DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/generated/modules
        )
    endforeach()
endif()

if(TARGET conflux)
    install(TARGETS conflux
        EXPORT confluxTargets-umbrella
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules
        FILE_SET generated_mods DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/generated/modules
    )
endif()

set(CONFLUX_INSTALL_NEEDS_LIBURING FALSE)
set(CONFLUX_INSTALL_NEEDS_XXHASH FALSE)
set(CONFLUX_INSTALL_NEEDS_LIBPQ FALSE)
set(CONFLUX_INSTALL_NEEDS_OPENSSL FALSE)
set(CONFLUX_INSTALL_NEEDS_ZLIB FALSE)
set(CONFLUX_INSTALL_NEEDS_LIBDEFLATE FALSE)
set(CONFLUX_INSTALL_NEEDS_ZLIB_NG FALSE)
set(CONFLUX_INSTALL_NEEDS_ISAL FALSE)
set(CONFLUX_INSTALL_NEEDS_BROTLI FALSE)
set(CONFLUX_INSTALL_NEEDS_ZSTD FALSE)
set(CONFLUX_INSTALL_NEEDS_NGHTTP2 FALSE)
set(CONFLUX_INSTALL_NEEDS_NGTCP2 FALSE)
set(CONFLUX_INSTALL_NEEDS_NGHTTP3 FALSE)
set(CONFLUX_INSTALL_MOCK_RUNTIME FALSE)

if(TARGET conflux_uring OR TARGET conflux_work OR TARGET conflux_process OR TARGET conflux_net_io_buffer OR TARGET conflux_file_io OR TARGET conflux_file_watch OR TARGET conflux_socket_io OR TARGET conflux_dns OR TARGET conflux_db OR TARGET conflux_http_server OR TARGET conflux_http_app OR TARGET conflux_net_http OR TARGET conflux)
    set(CONFLUX_INSTALL_NEEDS_LIBURING TRUE)
endif()
if(TARGET conflux_json)
    set(CONFLUX_INSTALL_NEEDS_XXHASH TRUE)
endif()
if(TARGET conflux_db)
    set(CONFLUX_INSTALL_NEEDS_LIBPQ TRUE)
endif()
if(TARGET conflux_net_tls OR (TARGET conflux_http_protocol AND CONFLUX_HAS_TLS STREQUAL "true") OR (TARGET conflux_http_server AND CONFLUX_HAS_TLS STREQUAL "true") OR (TARGET conflux_net_http AND CONFLUX_HAS_TLS STREQUAL "true") OR (TARGET conflux AND CONFLUX_HAS_TLS STREQUAL "true"))
    set(CONFLUX_INSTALL_NEEDS_OPENSSL TRUE)
endif()
if(TARGET conflux_http_compression AND CONFLUX_HAS_ZLIB STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_ZLIB TRUE)
endif()
if(TARGET conflux_http_compression AND CONFLUX_HAS_LIBDEFLATE STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_LIBDEFLATE TRUE)
endif()
if(TARGET conflux_http_compression AND CONFLUX_HAS_ZLIB_NG STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_ZLIB_NG TRUE)
endif()
if(TARGET conflux_http_compression AND CONFLUX_HAS_ISAL STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_ISAL TRUE)
endif()
if(TARGET conflux_http_compression AND CONFLUX_HAS_BROTLI STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_BROTLI TRUE)
endif()
if(TARGET conflux_http_compression AND CONFLUX_HAS_ZSTD STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_ZSTD TRUE)
endif()
if(TARGET conflux_http2)
    set(CONFLUX_INSTALL_NEEDS_NGHTTP2 TRUE)
endif()
if(TARGET conflux_http3)
    set(CONFLUX_INSTALL_NEEDS_NGTCP2 TRUE)
    set(CONFLUX_INSTALL_NEEDS_NGHTTP3 TRUE)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConfluxGeneratePackageMetadata.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ConfluxGeneratePackageMetadata.cmake"
    @ONLY
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/conflux-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
)
install(FILES
    ${CONFLUX_SRC_ROOT}/json_simd_backend.hxx
    ${CONFLUX_SRC_ROOT}/simd_backend.hxx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules/src
)
install(FILES
    ${CONFLUX_SRC_ROOT}/json_simd_backend.hxx
    ${CONFLUX_SRC_ROOT}/simd_backend.hxx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules
)
install(FILES
    ${CONFLUX_SRC_ROOT}/conflux/detail/discard.hxx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/detail
)
foreach(_target IN LISTS CONFLUX_INSTALL_INTERFACE_TARGETS CONFLUX_INSTALL_MODULE_TARGETS)
    get_target_property(_export_name ${_target} EXPORT_NAME)
    install(EXPORT confluxTargets-${_export_name}
        NAMESPACE conflux::
        FILE confluxTargets-${_export_name}.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
    )
endforeach()
if(TARGET conflux)
    install(EXPORT confluxTargets-umbrella
        NAMESPACE conflux::
        FILE confluxTargets-umbrella.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
    )
endif()
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/ConfluxGeneratePackageMetadata.cmake")

if(CONFLUX_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_test(NAME build/no-std-streams
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_no_std_streams.py")
    set_tests_properties(build/no-std-streams PROPERTIES
        LABELS "build;lint"
    )

    add_test(NAME build/cmake-source-files
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-cmake-source-files.py")
    set_tests_properties(build/cmake-source-files PROPERTIES
        LABELS "build;lint"
    )

    add_test(NAME build/component-map
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-component-map.py")
    set_tests_properties(build/component-map PROPERTIES
        LABELS "build;docs;package"
    )

    add_test(NAME build/package-config
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-package-config.sh"
                "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(build/package-config PROPERTIES
        LABELS "build;package"
    )

    add_test(NAME docs/planning-state
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-planning-state.py")
    set_tests_properties(docs/planning-state PROPERTIES
        LABELS "docs;lint"
    )

    add_test(NAME docs/release-docs
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-release-docs.py")
    set_tests_properties(docs/release-docs PROPERTIES
        LABELS "docs;lint"
    )

    add_test(NAME docs/package-docs
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-package-docs.py")
    set_tests_properties(docs/package-docs PROPERTIES
        LABELS "docs;package"
    )

    add_test(NAME docs/release-notes
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-release-notes.py")
    set_tests_properties(docs/release-notes PROPERTIES
        LABELS "docs;release"
    )

endif()

set(CONFLUX_PACKAGE_SMOKE_COMPONENTS "core" CACHE STRING
    "Semicolon-separated installed conflux components to validate with package smoke tests")
set(CONFLUX_PACKAGE_SMOKE_PREFIX "" CACHE PATH
    "Optional installed conflux prefix to validate with the package smoke project")
set(CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE "" CACHE STRING
    "Expected installed interface mode for package smoke tests, or empty to accept the package mode")
set_property(CACHE CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE PROPERTY STRINGS
    "" MODULE_INTERFACE HEADER_INTERFACE)
if(CONFLUX_BUILD_TESTS AND CONFLUX_PACKAGE_SMOKE_PREFIX)
    set(_conflux_package_smoke_args
        --source "${CMAKE_CURRENT_SOURCE_DIR}"
        --prefix "${CONFLUX_PACKAGE_SMOKE_PREFIX}"
        --build-dir "${CMAKE_CURRENT_BINARY_DIR}/package-smoke"
        --components "${CONFLUX_PACKAGE_SMOKE_COMPONENTS}")
    if(CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE)
        list(APPEND _conflux_package_smoke_args
            --interface-mode "${CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE}")
    endif()
    add_test(NAME build/package-config-install-tree
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-package-config-smoke.sh"
                ${_conflux_package_smoke_args})
    set_tests_properties(build/package-config-install-tree PROPERTIES
        LABELS "build;package;install"
    )
endif()

option(CONFLUX_RUN_INSTALL_TREE_SMOKE
    "Add an opt-in CTest that builds, installs, and consumes a fresh conflux install tree" OFF)
set(CONFLUX_INSTALL_TREE_SMOKE_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/install-tree-smoke-build" CACHE PATH
    "Build directory used by the opt-in install-tree smoke test")
set(CONFLUX_INSTALL_TREE_SMOKE_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/install-tree-smoke-prefix" CACHE PATH
    "Install prefix used by the opt-in install-tree smoke test")
set(CONFLUX_INSTALL_TREE_SMOKE_CONSUMER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/install-tree-smoke-consumer" CACHE PATH
    "Downstream consumer build directory used by the opt-in install-tree smoke test")
set(CONFLUX_INSTALL_TREE_SMOKE_FEATURE_SET "core" CACHE STRING
    "Feature preset used by the opt-in install-tree smoke test")
set(CONFLUX_INSTALL_TREE_SMOKE_BUILD_TYPE "Release" CACHE STRING
    "CMake build type used by the opt-in install-tree smoke test")
if(CONFLUX_BUILD_TESTS AND CONFLUX_RUN_INSTALL_TREE_SMOKE)
    add_test(NAME build/install-tree-smoke
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-install-tree-smoke.sh"
                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                --build-dir "${CONFLUX_INSTALL_TREE_SMOKE_BUILD_DIR}"
                --prefix "${CONFLUX_INSTALL_TREE_SMOKE_PREFIX}"
                --smoke-build-dir "${CONFLUX_INSTALL_TREE_SMOKE_CONSUMER_BUILD_DIR}"
                --feature-set "${CONFLUX_INSTALL_TREE_SMOKE_FEATURE_SET}"
                --build-type "${CONFLUX_INSTALL_TREE_SMOKE_BUILD_TYPE}"
                --components "${CONFLUX_PACKAGE_SMOKE_COMPONENTS}")
    set_tests_properties(build/install-tree-smoke PROPERTIES
        LABELS "build;package;install"
    )
endif()

# ---------------------------------------------------------------------------
# Standalone examples (don't require monolith)
# ---------------------------------------------------------------------------

if(CONFLUX_BUILD_EXAMPLES AND CONFLUX_WANT_FILE_IO)
    add_executable(conflux_file_io_example examples/advanced/file_io.cxx)
    target_link_libraries(conflux_file_io_example PRIVATE conflux_file_io conflux_work conflux_options)

    add_executable(conflux_coroutines examples/advanced/coroutines.cxx)
    target_link_libraries(conflux_coroutines PRIVATE conflux_file_io conflux_work conflux_options)
endif()

if(CONFLUX_BUILD_EXAMPLES AND CONFLUX_JSON_REFLECT AND TARGET conflux_json_reflect)
    add_executable(conflux_json_reflect_example examples/advanced/json_reflect.cxx)
    target_link_libraries(conflux_json_reflect_example
        PRIVATE conflux_json_reflect_provider conflux_json_reflect conflux_json conflux_types conflux_options)
endif()

if(CONFLUX_BUILD_EXAMPLES)
    set(CONFLUX_EXAMPLE_TARGETS
        conflux_quickstart_hello
        conflux_quickstart_json_crud
        conflux_quickstart_middleware
        conflux_quickstart_openapi
        conflux_quickstart_postgres
        conflux_quickstart_sse
        conflux_quickstart_static_files
        conflux_quickstart_websocket
        conflux_hello
        conflux_middleware
        conflux_sse
        conflux_static
        conflux_forms
        conflux_gzip
        conflux_http_client
        conflux_dual
        conflux_process_run_example
        conflux_crypto_sealing_example
        conflux_http_observability_example
        conflux_production_showcase_example
        conflux_http_policy_stack_example
        conflux_vhost_openapi_example
        conflux_http_client_builder_example
        conflux_api_typed_json_example
        conflux_http_client_json_example
        conflux_http_explicit_offload_example
        conflux_work_join_all_example
        conflux_template_pages_example
        conflux_h3_probe
        conflux_h3_server
        conflux_db_basic
        conflux_db_pool
        conflux_advanced_postgres
        conflux_json_example
        conflux_json_config_example
        conflux_json_stream_ingest_example
        conflux_json_diagnostics_example
        conflux_json_transform_example
        conflux_file_io_example
        conflux_coroutines
        conflux_json_reflect_example
    )
    set(CONFLUX_EXAMPLE_TARGET_REASONS
        "preview HTTP example"
        "requires JSON HTTP support"
        "preview HTTP example"
        "preview HTTP example"
        "requires DB support and libpq"
        "preview HTTP example"
        "preview HTTP example"
        "preview HTTP example"
        "preview HTTP example"
        "preview HTTP example"
        "preview HTTP example"
        "preview HTTP example"
        "preview HTTP example"
        "requires compression support"
        "HTTP client example"
        "HTTP server example"
        "process support example"
        "crypto support example"
        "observability example"
        "production showcase example"
        "HTTP policy example"
        "vhost/OpenAPI example"
        "HTTP client builder example"
        "requires JSON HTTP support"
        "requires JSON HTTP support"
        "requires work and JSON HTTP support"
        "work support example"
        "requires template support"
        "requires HTTP/3 support"
        "requires HTTP/3 support"
        "requires DB support and libpq"
        "requires DB support and libpq"
        "requires DB support and libpq"
        "JSON example"
        "JSON example"
        "JSON example"
        "JSON example"
        "JSON example"
        "requires file I/O support"
        "requires file I/O support"
        "requires JSON reflection support")
    set(CONFLUX_AVAILABLE_EXAMPLE_TARGETS)
    list(LENGTH CONFLUX_EXAMPLE_TARGETS _conflux_example_count)
    math(EXPR _conflux_example_last "${_conflux_example_count} - 1")
    foreach(_conflux_example_index RANGE 0 ${_conflux_example_last})
        list(GET CONFLUX_EXAMPLE_TARGETS ${_conflux_example_index} _conflux_example_target)
        list(GET CONFLUX_EXAMPLE_TARGET_REASONS ${_conflux_example_index} _conflux_example_reason)
        if(TARGET ${_conflux_example_target})
            list(APPEND CONFLUX_AVAILABLE_EXAMPLE_TARGETS ${_conflux_example_target})
            conflux_note_example(${_conflux_example_target} built "${_conflux_example_reason}")
        else()
            conflux_note_example(${_conflux_example_target} skipped "${_conflux_example_reason}")
        endif()
    endforeach()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT CONFLUX_GCC_EAGER_MODULE_IMPORTS)
        foreach(_conflux_example_target IN LISTS CONFLUX_AVAILABLE_EXAMPLE_TARGETS)
            target_compile_options(${_conflux_example_target} PRIVATE -fno-module-lazy)
        endforeach()
    endif()

    add_custom_target(conflux_examples)
    if(CONFLUX_AVAILABLE_EXAMPLE_TARGETS)
        add_dependencies(conflux_examples ${CONFLUX_AVAILABLE_EXAMPLE_TARGETS})
    endif()

    if(CONFLUX_BUILD_TESTS)
        add_test(NAME examples/quickstart-policy
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-quickstart-examples.sh"
                    "${CMAKE_CURRENT_SOURCE_DIR}/examples/quickstart"
        )
        set_tests_properties(examples/quickstart-policy PROPERTIES
            LABELS "examples;lint"
        )

        add_test(NAME examples/public-policy
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-public-examples.sh"
                    "${CMAKE_CURRENT_SOURCE_DIR}/examples"
        )
        set_tests_properties(examples/public-policy PROPERTIES
            LABELS "examples;lint"
        )

        add_test(NAME examples/compile
            COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                    --target conflux_examples --config "$<CONFIG>"
        )
        set_tests_properties(examples/compile PROPERTIES
            LABELS "examples;build"
            RUN_SERIAL TRUE
        )
    endif()
endif()
