cmake_minimum_required(VERSION 4.2)

set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")

project(conflux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_MODULE_STD ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(CONFLUX_ENABLE_ASAN    "Build with AddressSanitizer"                  OFF)
option(CONFLUX_ENABLE_UBSAN   "Build with UBSanitizer"                       OFF)
option(CONFLUX_ENABLE_TSAN    "Build with ThreadSanitizer"                   OFF)
option(CONFLUX_ENABLE_LTO     "Enable link-time optimisation"                OFF)
option(CONFLUX_BUILD_TESTS    "Build test targets"                           ON)
option(CONFLUX_BUILD_BENCHMARKS "Build benchmark targets"                    OFF)
option(CONFLUX_BUILD_FUZZ     "Build libFuzzer harness targets"              OFF)
option(CONFLUX_ENABLE_TLS     "Build TLS support (requires OpenSSL)"         ON)
option(CONFLUX_ENABLE_ZLIB    "Build classic zlib gzip backend"              ON)
option(CONFLUX_ENABLE_LIBDEFLATE "Build libdeflate gzip backend"             ON)
option(CONFLUX_ENABLE_ZLIB_NG "Build zlib-ng gzip backend"                   ON)
option(CONFLUX_ENABLE_ISAL    "Build ISA-L gzip backend"                     ON)
option(CONFLUX_ENABLE_BROTLI  "Build Brotli compression support"             ON)
option(CONFLUX_ENABLE_ZSTD    "Build Zstandard compression support"          ON)
option(CONFLUX_ENABLE_METRICS "Build Prometheus-compatible metrics support"  ON)
option(CONFLUX_ENABLE_HTTP2   "Build HTTP/2 support (requires nghttp2+TLS)"  ON)
option(CONFLUX_ENABLE_HTTP3   "Build HTTP/3 support (requires ngtcp2+nghttp3+OpenSSL 3.5+TLS)" ON)
option(CONFLUX_ENABLE_DB      "Build PostgreSQL client support (requires libpq)" ON)
option(CONFLUX_WORK_CARRIER_MODEL_A "Enable experimental work carrier Model A prototype surface" ON)
option(CONFLUX_WORK_CARRIER_MODEL_B "Enable experimental work carrier Model B prototype surface" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
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
    elseif(NOT OpenSSL_FOUND)
        message(STATUS "conflux: HTTP/2 disabled (TLS required)")
    else()
        message(STATUS "conflux: HTTP/2 disabled (CONFLUX_ENABLE_HTTP2=OFF)")
    endif()
endif()

# ---- HTTP/3 (requires ngtcp2 + nghttp3 + OpenSSL 3.5+ + TLS) ---------------
if(NGTCP2_FOUND AND NGTCP2_CRYPTO_OSSL_FOUND AND NGHTTP3_FOUND
        AND CONFLUX_HAS_TLS STREQUAL "true" AND CONFLUX_ENABLE_HTTP3)
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
    elseif(NOT OpenSSL_FOUND)
        message(STATUS "conflux: HTTP/3 disabled (TLS required)")
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

# Generate the conflux.features module from the template.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/conflux_features.cxx.in"
    "${CMAKE_CURRENT_BINARY_DIR}/src/conflux_features.cxx"
    @ONLY
)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(conflux_options INTERFACE)
conflux_apply_compiler_options(conflux_options)

add_library(conflux_types STATIC)
target_sources(conflux_types
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/types.cxx
)
target_link_libraries(conflux_types
    PRIVATE conflux_options
)

add_library(conflux_work STATIC)
target_sources(conflux_work
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/work/carrier_flags.cxx
        src/work/carrier_model_a.cxx
        src/work/carrier_model_b.cxx
        src/work/carrier_scope.cxx
        src/work/carrier_deadline.cxx
        src/work/carrier_coro.cxx
        src/work/carrier_streams.cxx
        src/work/carrier_timer.cxx
        src/work/root.cxx
        src/work.cxx
)
target_compile_definitions(conflux_work
    PUBLIC CONFLUX_WORK_CARRIER_MODEL_A=$<BOOL:${CONFLUX_WORK_CARRIER_MODEL_A}>
    PUBLIC CONFLUX_WORK_CARRIER_MODEL_B=$<BOOL:${CONFLUX_WORK_CARRIER_MODEL_B}>
)
target_link_libraries(conflux_work
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  PkgConfig::LIBURING
)

add_library(conflux_file_io STATIC)
target_sources(conflux_file_io
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/file_io/file_io.cxx
)
target_link_libraries(conflux_file_io
    PRIVATE conflux_options
    PUBLIC  conflux_work
    PUBLIC  PkgConfig::LIBURING
)

add_library(conflux_udp STATIC)
target_sources(conflux_udp
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/net/udp/udp.cxx
)
target_link_libraries(conflux_udp
    PRIVATE conflux_options
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  PkgConfig::LIBURING
)

add_library(conflux_dns STATIC)
target_sources(conflux_dns
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/net/dns/dns.cxx
)
target_link_libraries(conflux_dns
    PRIVATE conflux_options
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_udp
    PUBLIC  PkgConfig::LIBURING
)

if(CONFLUX_HAS_DB STREQUAL "true")
    add_library(conflux_db STATIC)
    target_sources(conflux_db
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES
            src/db/db.cxx
    )
    target_link_libraries(conflux_db
        PRIVATE conflux_options
        PUBLIC  conflux_work
        PUBLIC  conflux_file_io
        PUBLIC  PkgConfig::LIBURING
        PUBLIC  PkgConfig::LIBPQ
    )
endif()

add_library(conflux STATIC)

# Core module sources (always compiled).
set(CONFLUX_MODULE_SOURCES
    src/json.cxx
    src/file_watch.cxx
    src/template.cxx
    src/process.cxx
    src/utils.cxx
    src/crypto.cxx
    src/net/config.cxx
    src/conflux.cxx
    src/net/auth.cxx
    src/net/http_types.cxx
    src/net/http_request.cxx
    src/net/client.cxx
    src/net/compress.cxx
    src/net/rate_limit.cxx
    src/net/cors.cxx
    src/net/security.cxx
    src/net/forwarded.cxx
    src/net/request_id.cxx
    src/net/ip_filter.cxx
    src/net/cache_control.cxx
    src/net/trailing_slash.cxx
    src/net/redirect.cxx
    src/net/cookie_signing.cxx
    src/net/csrf.cxx
    src/net/etag.cxx
    src/net/response_cache.cxx
    src/net/structured_log.cxx
    src/net/tracing.cxx
    src/net/vhost.cxx
    src/net/proxy.cxx
    src/net/openapi.cxx
    src/net/http.cxx
    src/net/http1_parser.cxx
    src/net/http_server.cxx
    src/net/router.cxx
)

# Optional module sources.
if(CONFLUX_HAS_TLS STREQUAL "true")
    list(APPEND CONFLUX_MODULE_SOURCES src/net/tls.cxx)
    list(APPEND CONFLUX_MODULE_SOURCES src/net/smtp.cxx)
    list(APPEND CONFLUX_MODULE_SOURCES src/net/jwt.cxx)
endif()
if(CONFLUX_HAS_METRICS STREQUAL "true")
    list(APPEND CONFLUX_MODULE_SOURCES src/net/metrics.cxx)
endif()
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    list(APPEND CONFLUX_MODULE_SOURCES src/net/http2.cxx)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    list(APPEND CONFLUX_MODULE_SOURCES src/net/http3.cxx)
endif()
if(CONFLUX_HAS_ZLIB STREQUAL "true")
    list(APPEND CONFLUX_MODULE_SOURCES src/net/compress_backend_zlib.cxx)
endif()
if(CONFLUX_HAS_LIBDEFLATE STREQUAL "true")
    list(APPEND CONFLUX_MODULE_SOURCES src/net/compress_backend_libdeflate.cxx)
endif()
if(CONFLUX_HAS_ZLIB_NG STREQUAL "true")
    list(APPEND CONFLUX_MODULE_SOURCES src/net/compress_backend_zlibng.cxx)
endif()
if(CONFLUX_HAS_ISAL STREQUAL "true")
    list(APPEND CONFLUX_MODULE_SOURCES src/net/compress_backend_isal.cxx)
endif()

target_sources(conflux
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        ${CONFLUX_MODULE_SOURCES}
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
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_dns
    PUBLIC  PkgConfig::LIBURING
    PRIVATE xxhash
)

# TLS
if(CONFLUX_HAS_TLS STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_TLS=1)
    target_link_libraries(conflux PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_TLS=0)
endif()

# gzip
if(CONFLUX_HAS_COMPRESS STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_COMPRESS=1)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_COMPRESS=0)
endif()

if(CONFLUX_HAS_ZLIB STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_ZLIB=1)
    target_link_libraries(conflux PUBLIC ZLIB::ZLIB)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_ZLIB=0)
endif()

if(CONFLUX_HAS_LIBDEFLATE STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_LIBDEFLATE=1)
    target_link_libraries(conflux PUBLIC PkgConfig::LIBDEFLATE)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_LIBDEFLATE=0)
endif()

if(CONFLUX_HAS_ZLIB_NG STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_ZLIB_NG=1)
    target_link_libraries(conflux PUBLIC PkgConfig::ZLIB_NG)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_ZLIB_NG=0)
endif()

if(CONFLUX_HAS_ISAL STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_ISAL=1)
    target_link_libraries(conflux PUBLIC PkgConfig::LIBISAL)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_ISAL=0)
endif()

# brotli
if(CONFLUX_HAS_BROTLI STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_BROTLI=1)
    target_link_libraries(conflux PUBLIC PkgConfig::BROTLI)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_BROTLI=0)
endif()

# zstd
if(CONFLUX_HAS_ZSTD STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_ZSTD=1)
    target_link_libraries(conflux PUBLIC PkgConfig::ZSTD)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_ZSTD=0)
endif()

# metrics (no external dep)
if(CONFLUX_HAS_METRICS STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_METRICS=1)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_METRICS=0)
endif()

# HTTP/2
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_HTTP2=1)
    target_link_libraries(conflux PUBLIC PkgConfig::NGHTTP2)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_HTTP2=0)
endif()

# HTTP/3
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_HTTP3=1)
    target_link_libraries(conflux PUBLIC
        PkgConfig::NGTCP2
        PkgConfig::NGTCP2_CRYPTO_OSSL
        PkgConfig::NGHTTP3)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_HTTP3=0)
endif()

# Postgres
if(CONFLUX_HAS_DB STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_DB=1)
    target_link_libraries(conflux PUBLIC conflux_db)
else()
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_DB=0)
endif()

# Fuzz harness: propagate the macro so CONFLUX_FUZZ_EXPORT expands to `export`
# in module interfaces, making fuzzable internal helpers reachable from
# fuzz drivers. Otherwise stays empty and LTO/inlining are free to optimize.
if(CONFLUX_BUILD_FUZZ)
    target_compile_definitions(conflux PUBLIC CONFLUX_BUILD_FUZZ=1)
endif()

# ---------------------------------------------------------------------------
# Tests & examples
# ---------------------------------------------------------------------------

if(CONFLUX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CONFLUX_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(CONFLUX_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

add_executable(conflux_hello examples/hello.cxx)
target_link_libraries(conflux_hello PRIVATE conflux conflux_options)

add_executable(conflux_sse examples/sse.cxx)
target_link_libraries(conflux_sse PRIVATE conflux conflux_options)

add_executable(conflux_forms examples/forms.cxx)
target_link_libraries(conflux_forms PRIVATE conflux conflux_options)

add_executable(conflux_gzip examples/gzip.cxx)
target_link_libraries(conflux_gzip PRIVATE conflux conflux_options)

add_executable(conflux_dual examples/dual.cxx)
target_link_libraries(conflux_dual PRIVATE conflux conflux_options)

add_executable(conflux_http_client examples/http_client.cxx)
target_link_libraries(conflux_http_client PRIVATE conflux conflux_options)

add_executable(conflux_file_io_example examples/file_io.cxx)
target_link_libraries(conflux_file_io_example PRIVATE conflux_file_io conflux_work conflux_options)

add_executable(conflux_coroutines examples/coroutines.cxx)
target_link_libraries(conflux_coroutines PRIVATE conflux_file_io conflux_work conflux_options)

if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    add_executable(conflux_h3_probe examples/h3_probe.cxx)
    target_link_libraries(conflux_h3_probe PRIVATE conflux conflux_options)
    add_executable(conflux_h3_server examples/h3_server.cxx)
    target_link_libraries(conflux_h3_server PRIVATE conflux conflux_options)
endif()

add_executable(conflux_middleware examples/middleware.cxx)
target_link_libraries(conflux_middleware PRIVATE conflux conflux_options)

add_executable(conflux_static examples/static.cxx)
target_link_libraries(conflux_static PRIVATE conflux conflux_options)

if(CONFLUX_HAS_DB STREQUAL "true")
    add_executable(conflux_db_basic examples/db_basic.cxx)
    target_link_libraries(conflux_db_basic
        PRIVATE conflux_db conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)

    add_executable(conflux_db_pool examples/db_pool.cxx)
    target_link_libraries(conflux_db_pool
        PRIVATE conflux_db conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)
endif()
