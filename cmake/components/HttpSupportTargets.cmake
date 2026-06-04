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
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_parse_helpers
    PUBLIC  conflux_http_response
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
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_response
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
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_response
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
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_response
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
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_response
    PUBLIC  conflux_http_router
    PUBLIC  conflux_utils
    PUBLIC  conflux_work
)
endif() # CONFLUX_WANT_HTTP_VHOST || CONFLUX_WANT_HTTP_SERVER

if(CONFLUX_WANT_HTTP_COMPRESSION)
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
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_parse_helpers
    PUBLIC  conflux_http_response
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
endif() # CONFLUX_WANT_HTTP_COMPRESSION

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
