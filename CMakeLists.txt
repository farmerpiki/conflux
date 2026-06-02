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
include(cmake/components/FileTargets.cmake)
include(cmake/components/TemplateTargets.cmake)
include(cmake/components/RuntimeTargets.cmake)
include(cmake/components/DnsTargets.cmake)
include(cmake/components/DbTargets.cmake)

include(ConfluxComponentValidation)

include(cmake/components/HttpCoreTargets.cmake)
include(cmake/components/NetworkTargets.cmake)
include(cmake/components/HttpRouterTargets.cmake)
include(cmake/components/HttpSupportTargets.cmake)

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
set(_conflux_http_server_private_partitions
    ${CONFLUX_SRC_ROOT}/net/http_server_state.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_tls.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_send.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_ws.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_recv.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_cqe.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_diag.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_loop.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_dispatch.cxx
)
conflux_apply_http_server_compiler_workarounds("${CONFLUX_SRC_ROOT}/net/http_server_send.cxx")
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    list(APPEND _conflux_http_server_private_partitions
        ${CONFLUX_SRC_ROOT}/net/http_server_h2.cxx
    )
endif()
target_sources(conflux_http_server
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_server.cxx
)
target_sources(conflux_http_server
    PRIVATE FILE_SET private_http_server_partitions TYPE CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${_conflux_http_server_private_partitions}
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
            ${CONFLUX_SRC_ROOT}/net/app_policies.cxx
            ${CONFLUX_SRC_ROOT}/net/app_response.cxx
            ${CONFLUX_SRC_ROOT}/net/app_route_helpers.cxx
            ${CONFLUX_SRC_ROOT}/net/app_traits.cxx
            ${CONFLUX_SRC_ROOT}/net/app_types.cxx
            ${CONFLUX_SRC_ROOT}/net/app.cxx
)
target_link_libraries(conflux_http_app
    PRIVATE conflux_options
    PRIVATE conflux_file_io_sync
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
    PUBLIC  conflux_utils
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
add_library(conflux_net_http STATIC)
target_sources(conflux_net_http
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
            ${CONFLUX_SRC_ROOT}/net/http_facade_problem.cxx
            ${CONFLUX_SRC_ROOT}/net/http_facade.cxx
            ${CONFLUX_SRC_ROOT}/net/http_facade_extended.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_umbrella.cxx
            ${CONFLUX_SRC_ROOT}/net/http.cppm
)
target_link_libraries(conflux_net_http
    PUBLIC conflux_types
    PUBLIC conflux_net_config
    PUBLIC conflux_http_core
    PUBLIC conflux_http_json
    PUBLIC conflux_http_response_json
    PUBLIC conflux_http_app_json
    PUBLIC conflux_http_router
    PUBLIC conflux_net_client
    PUBLIC conflux_http_protocol
    PUBLIC conflux_http_server
    PUBLIC conflux_http_app
    PUBLIC conflux_http_auth
    PUBLIC conflux_http_compression
    PUBLIC conflux_http_policy
    PUBLIC conflux_http_observability
    PUBLIC conflux_http_openapi
    PUBLIC conflux_http_vhost
)
if(TARGET conflux_http_native_json)
    target_link_libraries(conflux_net_http PUBLIC conflux_http_native_json)
endif()
if(TARGET conflux_json)
    target_link_libraries(conflux_net_http PUBLIC conflux_json)
endif()
if(CONFLUX_HAS_TLS STREQUAL "true")
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_TLS=1)
else()
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_TLS=0)
endif()
if(CONFLUX_HAS_METRICS STREQUAL "true")
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_METRICS=1)
else()
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_METRICS=0)
endif()
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_HTTP2=1)
else()
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_HTTP2=0)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_HTTP3=1)
else()
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_HTTP3=0)
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
        ${CONFLUX_SRC_ROOT}/facade/conflux_curated.cxx
        ${CONFLUX_SRC_ROOT}/facade/conflux_extended.cxx
        ${CONFLUX_SRC_ROOT}/facade/conflux_complete.cxx
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
if(TARGET conflux_json_reflect)
    target_link_libraries(conflux PUBLIC conflux_json_reflect)
endif()
if(TARGET conflux_json_reflect_provider)
    target_link_libraries(conflux PUBLIC conflux_json_reflect_provider)
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
if(TARGET conflux_pg)
    target_link_libraries(conflux PUBLIC conflux_pg)
endif()

if(CONFLUX_JSON_AVX2)
    target_compile_options(conflux PRIVATE -mavx2 -mbmi2)
endif()

# Template engine / hot-reload surface. The file-watch module is now compiled
# by conflux_file_watch; the umbrella module only re-exports it when present.
conflux_apply_api_surface_definitions(conflux PUBLIC)
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
# `conflux::http_protocol`, `conflux::pg`, ...). Keep the aggregate target
# thin so optional packages are not pulled in just because the umbrella module
# is built.

# Fuzz harness: propagate the macro so CONFLUX_FUZZ_EXPORT expands to `export`
# in module interfaces, making fuzzable internal helpers reachable from
# fuzz drivers. Otherwise stays empty and LTO/inlining are free to optimize.
if(CONFLUX_BUILD_FUZZ)
    target_compile_definitions(conflux PUBLIC CONFLUX_BUILD_FUZZ=1)
endif()

endif() # CONFLUX_WANT_HTTP_SERVER

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
