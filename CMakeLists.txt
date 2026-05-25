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

include(cmake/ConfluxOptions.cmake)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ConfluxPresets)
conflux_apply_preset()

# Provider AUTO is preset-owned. Keep cache entries as the user-facing request
# and resolve local values before any dependency discovery happens.
set(CONFLUX_JSON_HASH_PROVIDER "${CONFLUX_EFFECTIVE_JSON_HASH_PROVIDER}")
set(CONFLUX_GZIP_PROVIDER "${CONFLUX_EFFECTIVE_GZIP_PROVIDER}")
set(CONFLUX_BROTLI_PROVIDER "${CONFLUX_EFFECTIVE_BROTLI_PROVIDER}")
set(CONFLUX_ZSTD_PROVIDER "${CONFLUX_EFFECTIVE_ZSTD_PROVIDER}")
set(CONFLUX_TLS_PROVIDER "${CONFLUX_EFFECTIVE_TLS_PROVIDER}")
set(CONFLUX_HTTP2_PROVIDER "${CONFLUX_EFFECTIVE_HTTP2_PROVIDER}")
set(CONFLUX_HTTP3_PROVIDER "${CONFLUX_EFFECTIVE_HTTP3_PROVIDER}")
set(CONFLUX_POSTGRES_PROVIDER "${CONFLUX_EFFECTIVE_POSTGRES_PROVIDER}")
set(CONFLUX_PASSWORD_HASH_ARGON2_PROVIDER "${CONFLUX_EFFECTIVE_PASSWORD_HASH_ARGON2_PROVIDER}")

include(CompilerOptions)
include(ConfluxSimd)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(Python3_FIND_IMPLEMENTATIONS CPython)
set(Python3_FIND_STRATEGY LOCATION)
if(NOT Python3_EXECUTABLE)
    find_program(_CONFLUX_DEFAULT_PYTHON3
        NAMES python3
        PATHS /usr/bin /usr/local/bin /bin
        NO_DEFAULT_PATH)
    if(_CONFLUX_DEFAULT_PYTHON3)
        set(Python3_EXECUTABLE "${_CONFLUX_DEFAULT_PYTHON3}" CACHE FILEPATH
            "Python 3 interpreter used by Conflux configure-time scripts")
    endif()
endif()
include(ConfluxInterfaceMode)
conflux_configure_interface_mode()

if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
    include(cmake/ConfluxHeaderInterface.cmake)
    return()
endif()

include(Dependencies)

include(ConfluxFeatures)
conflux_detect_aesni(CONFLUX_HAS_AESNI)

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
    CONFLUX_MODULE_USE_IMPORT_STD=$<BOOL:${CONFLUX_IMPORT_STD_ENABLED}>
    CONFLUX_MODULE_USE_IMPORT_STD_COMPAT=0
    CONFLUX_MODULE_USE_IMPORTS=0
    CONFLUX_MODULE_REEXPORT_IMPORTS=0
    CONFLUX_ENABLE_EXPERIMENTAL=$<BOOL:${CONFLUX_ENABLE_EXPERIMENTAL}>
    CONFLUX_CPU_FEATURE_PROBES_RUNTIME=$<BOOL:${_conflux_cpu_feature_probes_runtime}>
    CONFLUX_SIMD_SELECTION_DIRECT=$<BOOL:${_conflux_simd_selection_direct}>
    CONFLUX_SIMD_SELECTION_RUNTIME=$<BOOL:${_conflux_simd_selection_runtime}>
    CONFLUX_ENABLE_RECV_BUNDLE=$<BOOL:${CONFLUX_ENABLE_RECV_BUNDLE}>
    CONFLUX_ENABLE_RECV_INCREMENTAL_BUF=$<BOOL:${CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF}>
    CONFLUX_ENABLE_SEND_ZC=$<BOOL:${CONFLUX_EXPERIMENTAL_SEND_ZC}>
    CONFLUX_ENABLE_RING_GROWTH=$<BOOL:${CONFLUX_EXPERIMENTAL_RING_GROWTH}>
    CONFLUX_ENABLE_IOPOLL_STORAGE_TEST=$<BOOL:${CONFLUX_EXPERIMENTAL_IOPOLL_STORAGE_TEST}>
    CONFLUX_USE_MOCK_LIBURING=$<BOOL:${CONFLUX_USE_MOCK_LIBURING}>
    CONFLUX_HAS_WARNING_CLEAN_AUTO_UNDERSCORE_DISCARD=$<BOOL:${CONFLUX_HAS_WARNING_CLEAN_AUTO_UNDERSCORE_DISCARD}>
    CONFLUX_HAS_JSON_REFLECT=$<TARGET_EXISTS:conflux_json_reflect>
    CONFLUX_BUILD_VERSION="${PROJECT_VERSION}"
    CONFLUX_BUILD_GIT_COMMIT="${CONFLUX_GIT_COMMIT}"
    CONFLUX_BUILD_COMPILER="${CMAKE_CXX_COMPILER_ID}"
    CONFLUX_BUILD_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    CONFLUX_BUILD_STDLIB="${CONFLUX_STDLIB_NAME}"
    CONFLUX_BUILD_STDLIB_VERSION="${CONFLUX_STDLIB_VERSION}"
    CONFLUX_BUILD_TYPE_VALUE="${CMAKE_BUILD_TYPE}"
    CONFLUX_BUILD_INTERFACE_MODE="${CONFLUX_INTERFACE_MODE}"
    CONFLUX_BUILD_FEATURE_SET="${CONFLUX_FEATURE_SET}"
    CONFLUX_BUILD_API_SURFACE="${CONFLUX_API_SURFACE}"
    CONFLUX_BUILD_SIMD_SELECTION="${_conflux_simd_selection}"
    CONFLUX_BUILD_CPU_FEATURE_PROBES_RUNTIME=$<BOOL:${_conflux_cpu_feature_probes_runtime}>
)
conflux_apply_api_surface_definitions(conflux_options INTERFACE)
if(CONFLUX_ENABLE_EXPERIMENTAL)
    message(STATUS "conflux: experimental features enabled")
else()
    message(STATUS "conflux: experimental features disabled")
endif()
message(STATUS "conflux: SIMD selection resolved to ${_conflux_simd_selection}")
if(_conflux_cpu_feature_probes_runtime)
    message(STATUS "conflux: CPU feature probes use runtime detection")
else()
    message(STATUS "conflux: CPU feature probes assume selected compiled ISA is available")
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
        ${CONFLUX_SRC_ROOT}/small_function.cxx
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
if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    target_compile_definitions(conflux_core INTERFACE CONFLUX_INTERFACE_MODULE=1)
endif()

add_library(conflux_cpu_features STATIC ${CONFLUX_SRC_ROOT}/cpu_features.cxx)
target_link_libraries(conflux_cpu_features PRIVATE conflux_options)

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
    PRIVATE conflux_cpu_features
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
        ${CONFLUX_SRC_ROOT}/json_codec.cxx
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
    PRIVATE conflux_options
    PRIVATE conflux_cpu_features
)
if(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "XXHASH")
    target_link_libraries(conflux_json PRIVATE PkgConfig::XXHASH)
    target_compile_definitions(conflux_json PRIVATE CONFLUX_JSON_HASH_PROVIDER_XXHASH=1)
elseif(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "INTERNAL")
    target_compile_definitions(conflux_json PRIVATE CONFLUX_JSON_HASH_PROVIDER_INTERNAL=1)
endif()

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
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_TLS_PROVIDER=AUTO/OPENSSL and OpenSSL")
endif()

# conflux.net.config is part of the core surface and imports conflux.file_io_sync
# for config/secret file loading, so the provider module is required even when
# broader runtime file I/O is disabled.
if(NOT CONFLUX_WANT_FILE_IO_SYNC)
    set(CONFLUX_WANT_FILE_IO_SYNC TRUE)
    set(CONFLUX_WANT_FILE_IO_SYNC TRUE CACHE INTERNAL "Resolved conflux component flag" FORCE)
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
            ${CONFLUX_SRC_ROOT}/net/http_json_string.cxx
            ${CONFLUX_SRC_ROOT}/net/http_request.cxx
            ${CONFLUX_SRC_ROOT}/net/server_types.cxx
    )
    target_link_libraries(conflux_http_core
        PRIVATE conflux_options
        PUBLIC  conflux_types
        PUBLIC  conflux_utils
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

add_library(conflux_uring_primitives STATIC)
target_sources(conflux_uring_primitives
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/uring/uring_fd.cxx
        ${CONFLUX_SRC_ROOT}/uring/uring_fd_compat.cxx
        ${CONFLUX_SRC_ROOT}/uring/uring_sqe.cxx
)
target_link_libraries(conflux_uring_primitives
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  PkgConfig::LIBURING
)

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
    PUBLIC  conflux_uring_primitives
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
        ${CONFLUX_SRC_ROOT}/work/root_core.cxx
        ${CONFLUX_SRC_ROOT}/work/root_tasks.cxx
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
    PUBLIC  conflux_uring_primitives
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
target_sources(conflux_template
    PRIVATE
        ${CONFLUX_SRC_ROOT}/template_env.cxx
        ${CONFLUX_SRC_ROOT}/template_impl.cxx
        ${CONFLUX_SRC_ROOT}/template_parse.cxx
        ${CONFLUX_SRC_ROOT}/template_render.cxx
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC 15 currently ICEs in the ealias pass on this C++ module implementation
    # unit at release optimization levels. Function-level optimize attributes are
    # ignored for modules, so keep the workaround scoped to this source file.
    set_source_files_properties(${CONFLUX_SRC_ROOT}/template_impl.cxx PROPERTIES COMPILE_OPTIONS "-O0")
endif()
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
        ${CONFLUX_SRC_ROOT}/socket_io/tcp_listener.cxx
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
        PUBLIC  conflux_uring
        PUBLIC  conflux_uring_timeout
        PUBLIC  conflux_file_io_sync
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

conflux_detect_stdsimd(_conflux_simd_backend)
conflux_add_stdsimd_targets("${_conflux_simd_backend}")
conflux_attach_stdsimd_targets("${_conflux_simd_backend}")

add_library(conflux_utils STATIC)
target_sources(conflux_utils
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/utils.cxx
        ${CONFLUX_SRC_ROOT}/facade/conflux_core.cxx
)
target_link_libraries(conflux_utils
    PRIVATE conflux_options
    PRIVATE conflux_cpu_features
    PUBLIC  conflux_types
)
target_link_libraries(conflux_core INTERFACE conflux_utils)
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
    foreach(_conflux_json_reflect_dep IN ITEMS
            conflux_types
            conflux_json
            conflux_json_boundary
            conflux_json_native_provider)
        if(TARGET ${_conflux_json_reflect_dep})
            target_compile_options(${_conflux_json_reflect_dep} PUBLIC -freflection)
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
        FILES ${CONFLUX_SRC_ROOT}/net/openapi.cxx
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
target_sources(conflux_http_app
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
            ${CONFLUX_SRC_ROOT}/net/http_facade_problem.cxx
            ${CONFLUX_SRC_ROOT}/net/http_facade.cxx
            ${CONFLUX_SRC_ROOT}/net/http_facade_extended.cxx
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
if(TARGET conflux_db)
    target_link_libraries(conflux PUBLIC conflux_db)
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
# `conflux::http_protocol`, `conflux::db`, ...). Keep the aggregate target
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
