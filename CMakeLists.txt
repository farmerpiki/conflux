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

if(CONFLUX_WANT_HTTP_CORE OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
    conflux_add_module_library(conflux_http_core
        PUBLIC_MODULES
            ${CONFLUX_SRC_ROOT}/net/http_types.cxx
            ${CONFLUX_SRC_ROOT}/net/http_json_string.cxx
            ${CONFLUX_SRC_ROOT}/net/http_request.cxx
            ${CONFLUX_SRC_ROOT}/net/server_types.cxx
    )
    target_link_libraries(conflux_http_core
        PRIVATE conflux_options
        PUBLIC  conflux_crypto
        PUBLIC  conflux_types
        PUBLIC  conflux_utils
    )
endif()

if(CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
    conflux_add_module_library(conflux_http_json
        PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/net/http_json.cxx
    )
    target_link_libraries(conflux_http_json
        PRIVATE conflux_options
        PUBLIC  conflux_http_core
        PUBLIC  conflux_json_boundary
    )
endif()

add_library(conflux_net_config STATIC)
target_sources(conflux_net_config
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/config.cxx
)
target_link_libraries(conflux_net_config
    PRIVATE conflux_options
    PUBLIC  conflux_file_io_sync
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
    foreach(_conflux_json_reflect_dep IN ITEMS
            conflux_types
            conflux_json
            conflux_json_boundary
            conflux_json_native_provider)
        if(TARGET ${_conflux_json_reflect_dep})
            target_compile_options(${_conflux_json_reflect_dep} PUBLIC ${CONFLUX_REFLECTION_COMPILE_OPTIONS})
        endif()
    endforeach()

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
    target_compile_options(conflux_json_reflect PUBLIC ${CONFLUX_REFLECTION_COMPILE_OPTIONS})

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
    target_compile_options(conflux_json_reflect_provider PUBLIC ${CONFLUX_REFLECTION_COMPILE_OPTIONS})
    message(STATUS "conflux: JSON P2996 reflection codec enabled (${CONFLUX_REFLECTION_COMPILE_OPTIONS})")
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
    PRIVATE conflux_cpu_features
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
        FILES
            ${CONFLUX_SRC_ROOT}/net/path.cxx
            ${CONFLUX_SRC_ROOT}/net/router.cxx
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
target_compile_definitions(conflux_http_router_dispatch PRIVATE
    CONFLUX_ROUTER_LAZY_ROUTE_METADATA=$<BOOL:${CONFLUX_ROUTER_LAZY_ROUTE_METADATA}>)
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
    PRIVATE conflux_file_io_sync
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
    PRIVATE conflux_socket_io
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
    PUBLIC  conflux_http_parse_helpers
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
if(CONFLUX_HAS_TLS STREQUAL "true")
    if(NOT TARGET conflux_json)
        message(FATAL_ERROR "conflux: TLS JWT auth requires CONFLUX_BUILD_JSON")
    endif()
    target_link_libraries(conflux_http_auth PUBLIC conflux_json)
endif()
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
    set(CONFLUX_RESOLVED_ARGON2_PROVIDER "SYSTEM" CACHE STRING "Resolved Argon2 password-hashing provider" FORCE)
    message(STATUS "conflux: password hashing Argon2 backend linked (${ARGON2_VERSION})")
elseif(NOT CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "OFF")
    target_compile_definitions(conflux_http_auth PRIVATE CONFLUX_PASSWORD_HASH_ARGON2_RUNTIME=1)
    if(CMAKE_DL_LIBS)
        target_link_libraries(conflux_http_auth PRIVATE ${CMAKE_DL_LIBS})
    endif()
    set(CONFLUX_RESOLVED_ARGON2_PROVIDER "RUNTIME" CACHE STRING "Resolved Argon2 password-hashing provider" FORCE)
    message(STATUS "conflux: password hashing Argon2 backend uses runtime loader")
else()
    set(CONFLUX_RESOLVED_ARGON2_PROVIDER "OFF" CACHE STRING "Resolved Argon2 password-hashing provider" FORCE)
    message(STATUS "conflux: password hashing Argon2 backend disabled")
endif()
if(CONFLUX_HAS_TLS STREQUAL "true")
    target_compile_definitions(conflux_http_auth PUBLIC CONFLUX_HAS_TLS=1)
else()
    target_compile_definitions(conflux_http_auth PUBLIC CONFLUX_HAS_TLS=0)
endif()
endif() # CONFLUX_WANT_HTTP_AUTH || CONFLUX_WANT_HTTP_SERVER
if(NOT (CONFLUX_WANT_HTTP_AUTH OR CONFLUX_WANT_HTTP_SERVER))
    set(CONFLUX_RESOLVED_ARGON2_PROVIDER "OFF" CACHE STRING "Resolved Argon2 password-hashing provider" FORCE)
endif()

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
    PRIVATE conflux_file_io_sync
    PUBLIC  conflux_http_router
    PUBLIC  conflux_utils
    PUBLIC  conflux_work
)
if(TARGET conflux_json)
    target_link_libraries(conflux_http_observability PUBLIC conflux_json)
    target_compile_definitions(conflux_http_observability PUBLIC CONFLUX_HAS_JSON=1)
else()
    target_compile_definitions(conflux_http_observability PUBLIC CONFLUX_HAS_JSON=0)
endif()
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
        FILES
            ${CONFLUX_SRC_ROOT}/net/app_openapi.cxx
            ${CONFLUX_SRC_ROOT}/net/openapi.cxx
)
target_link_libraries(conflux_http_openapi
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
    PUBLIC  conflux_utils
)
if(TARGET conflux_json)
    target_link_libraries(conflux_http_openapi PUBLIC conflux_json)
    target_compile_definitions(conflux_http_openapi PUBLIC CONFLUX_HAS_JSON=1)
else()
    target_compile_definitions(conflux_http_openapi PUBLIC CONFLUX_HAS_JSON=0)
endif()
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
    PUBLIC  conflux_http_parse_helpers
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
    PUBLIC  conflux_http_parse_helpers
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
