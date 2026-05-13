cmake_minimum_required(VERSION 3.30)

if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3.0")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.2")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.0" OR CMAKE_VERSION MATCHES "^3\\.31\\.")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "a9e1cf81-9932-4810-974b-6eccaf14e457")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28.0")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
else()
    message(WARNING "Your CMake version does not support experimental 'import std'.")
endif()

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
option(CONFLUX_JSON_REFLECT   "Enable P2996 reflection codec (GCC 16+ only)" OFF)
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
option(CONFLUX_BUILD_FUZZ     "Build libFuzzer harness targets"              OFF)
option(CONFLUX_ENABLE_TLS     "Build TLS support (requires OpenSSL)"         ON)
option(CONFLUX_ENABLE_ZLIB    "Build classic zlib gzip backend"              ON)
option(CONFLUX_ENABLE_LIBDEFLATE "Build libdeflate gzip backend"             ON)
option(CONFLUX_ENABLE_ZLIB_NG "Build zlib-ng gzip backend"                   ON)
option(CONFLUX_ENABLE_ISAL    "Build ISA-L gzip backend"                     ON)
option(CONFLUX_ENABLE_BROTLI  "Build Brotli compression support"             ON)
option(CONFLUX_ENABLE_ZSTD    "Build Zstandard compression support"          ON)
option(CONFLUX_ENABLE_METRICS "Build Prometheus-compatible metrics support"  ON)
option(CONFLUX_ENABLE_RECV_BUNDLE "Allow io_uring multishot recv bundling when configured and supported by the kernel" ON)
option(CONFLUX_ENABLE_HTTP2   "Build HTTP/2 support (requires nghttp2+TLS)"  ON)
option(CONFLUX_ENABLE_HTTP3   "Build HTTP/3 support (requires ngtcp2+nghttp3+OpenSSL 3.5+TLS)" ON)
option(CONFLUX_ENABLE_DB      "Build PostgreSQL client support (requires libpq)" ON)
option(CONFLUX_WORK_CARRIER_MODEL_A "Enable experimental work carrier Model A prototype surface" ON)
option(CONFLUX_WORK_CARRIER_MODEL_B "Enable experimental work carrier Model B prototype surface" OFF)
option(CONFLUX_SUPPRESS_DEPRECATION_WARNINGS "Temporarily suppress deprecated API warnings during pre-v1 migration" ON)
option(CONFLUX_WORK_CORO_FRAME_POOL "Enable per-thread monotonic bump arena for EagerChain coroutine frames" OFF)
option(CONFLUX_WORK_ALLOC_STATS "Enable relaxed allocation counters for work.root task/control-block diagnostics" OFF)
option(CONFLUX_GCC_EAGER_MODULE_IMPORTS "Work around GCC module CMI lazy-load corruption by using -fno-module-lazy on GCC 16+" ON)
option(CONFLUX_GCC_SUGGEST_ATTRIBUTES "Enable noisy GCC -Wsuggest-attribute=pure/const optimization hints" OFF)
set(CONFLUX_JSON_USE_STDSIMD "AUTO" CACHE STRING "Use std::experimental::simd for JSON SIMD paths (AUTO|ON|OFF)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ConfluxPresets)
conflux_apply_preset()
include(CompilerOptions)
include(Dependencies)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(conflux_options INTERFACE)
conflux_apply_compiler_options(conflux_options)
target_compile_definitions(conflux_options INTERFACE
    CONFLUX_ENABLE_RECV_BUNDLE=$<BOOL:${CONFLUX_ENABLE_RECV_BUNDLE}>
)
if(CONFLUX_ENABLE_RECV_BUNDLE)
    message(STATUS "conflux: recv bundle build gate enabled")
else()
    message(STATUS "conflux: recv bundle build gate disabled")
endif()

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

# First slice of the public component model: a liburing-free core target.
# `conflux_types` remains the concrete module carrier; `conflux_core` is the
# stable package-facing dependency anchor for consumers that only need core
# vocabulary/error types.
add_library(conflux_core INTERFACE)
target_link_libraries(conflux_core INTERFACE conflux_types)

if(CONFLUX_WANT_CRYPTO)
add_library(conflux_crypto STATIC)
target_sources(conflux_crypto
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/crypto.cxx
)
target_link_libraries(conflux_crypto
    PRIVATE conflux_options
    PUBLIC  conflux_types
)
if(CONFLUX_HAS_AESNI)
    message(STATUS "conflux: AES-NI + PCLMUL + SSSE3 path enabled")
    target_compile_definitions(conflux_crypto PRIVATE CONFLUX_CRYPTO_USE_AESNI=1)
    target_sources(conflux_crypto PRIVATE src/crypto_aesni.cxx)
    set_source_files_properties(src/crypto_aesni.cxx PROPERTIES
        COMPILE_OPTIONS "-maes;-mpclmul;-mssse3;-msse4.1"
    )
endif()
endif()

if(CONFLUX_WANT_JSON)
add_library(conflux_json STATIC)
target_sources(conflux_json
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/json.cxx
)
target_link_libraries(conflux_json
    PUBLIC  conflux_types
    PRIVATE conflux_options PkgConfig::XXHASH
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
if((CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER) AND NOT TARGET conflux_json)
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_HTTP_JSON/CONFLUX_BUILD_HTTP_SERVER requires CONFLUX_BUILD_JSON")
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
if(CONFLUX_WANT_SMTP AND CONFLUX_HAS_TLS STREQUAL "false")
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

if(CONFLUX_WANT_FILE_IO_SYNC)
add_library(conflux_file_io_sync STATIC)
target_sources(conflux_file_io_sync
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/file_io/file_io_sync.cxx
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
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/file_io/file_map.cxx
)
target_link_libraries(conflux_file_map
    PRIVATE conflux_options
    PUBLIC  conflux_file_io_sync
)
endif() # CONFLUX_WANT_FILE_MAP

if(CONFLUX_WANT_JSON_FILE)
add_library(conflux_json_file STATIC)
target_sources(conflux_json_file
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/json_file.cxx
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
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES
            src/net/http_types.cxx
            src/net/http_request.cxx
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
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES src/net/http_json.cxx
    )
    target_link_libraries(conflux_http_json
        PRIVATE conflux_options
        PUBLIC  conflux_http_core
        PUBLIC  conflux_json
    )
endif()

if(CONFLUX_NEEDS_RUNTIME)

add_library(conflux_uring STATIC)
target_sources(conflux_uring
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/uring/uring.cxx
        src/uring/flow.cxx
        src/uring/uring_completion.cxx
        src/uring/uring_handle.cxx
)
target_link_libraries(conflux_uring
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  PkgConfig::LIBURING
)

# Probe placed after include(Dependencies) so PkgConfig::LIBURING is available.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES PkgConfig::LIBURING)
check_cxx_source_compiles("
#include <liburing.h>
int main() {
    auto* fn = &io_uring_resize_rings;
    (void)fn;
    return 0;
}
" CONFLUX_HAVE_IO_URING_RESIZE_RINGS)
unset(CMAKE_REQUIRED_LIBRARIES)
target_compile_definitions(conflux_uring
    PUBLIC CONFLUX_HAVE_IO_URING_RESIZE_RINGS=$<BOOL:${CONFLUX_HAVE_IO_URING_RESIZE_RINGS}>
)

add_library(conflux_work STATIC)
target_sources(conflux_work
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/work/carrier_flags.cxx
        src/work/carrier_model_a.cxx
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
    PUBLIC CONFLUX_WORK_CORO_FRAME_POOL=$<BOOL:${CONFLUX_WORK_CORO_FRAME_POOL}>
    PUBLIC CONFLUX_WORK_ALLOC_STATS=$<BOOL:${CONFLUX_WORK_ALLOC_STATS}>
)
target_link_libraries(conflux_work
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  PkgConfig::LIBURING
)

endif() # CONFLUX_NEEDS_RUNTIME (uring + work)

if(CONFLUX_WANT_FILE_IO)
add_library(conflux_file_io STATIC)
target_sources(conflux_file_io
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/file_io/file_io.cxx
)
target_link_libraries(conflux_file_io
    PRIVATE conflux_options
    PUBLIC  conflux_file_io_sync
    PUBLIC  conflux_file_map
    PUBLIC  conflux_uring
    PUBLIC  conflux_work
    PUBLIC  PkgConfig::LIBURING
)

endif() # CONFLUX_WANT_FILE_IO

if(CONFLUX_WANT_FILE_WATCH)
add_library(conflux_file_watch STATIC)
target_sources(conflux_file_watch
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/file_watch.cxx
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
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/template.cxx
)
target_compile_definitions(conflux_template
    PUBLIC CONFLUX_HAS_FILE_WATCH=$<BOOL:${CONFLUX_WANT_TEMPLATES_WATCH}>
)
target_link_libraries(conflux_template
    PRIVATE conflux_options
    PUBLIC  conflux_json
)
if(CONFLUX_WANT_TEMPLATES_WATCH)
    target_link_libraries(conflux_template PUBLIC conflux_file_watch)
endif()
endif() # CONFLUX_WANT_TEMPLATES

if(CONFLUX_WANT_SOCKET_IO)
add_library(conflux_socket_io STATIC)
target_sources(conflux_socket_io
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/socket_io/socket_io.cxx
        src/socket_io/socket_io_coro.cxx
        src/socket_io/socket_io_blocking.cxx
)
target_sources(conflux_socket_io
    PRIVATE
        src/socket_io/socket_io_coro_impl.cxx
)

target_link_libraries(conflux_socket_io
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_uring
    PUBLIC  conflux_work
    PUBLIC  PkgConfig::LIBURING
)

endif() # CONFLUX_WANT_SOCKET_IO

if(CONFLUX_NEEDS_RUNTIME)
add_library(conflux_net_cancel STATIC)
target_sources(conflux_net_cancel
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/cancel.cxx
)
target_sources(conflux_net_cancel
    PRIVATE
        src/net/cancel_impl.cxx
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
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/net/dns/dns.cxx
)
target_sources(conflux_dns
    PRIVATE
        src/net/dns/dns_impl.cxx
)
target_link_libraries(conflux_dns
    PRIVATE conflux_options
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_socket_io
    PUBLIC  PkgConfig::LIBURING
)

endif() # CONFLUX_WANT_DNS

if(CONFLUX_HAS_DB STREQUAL "true" AND CONFLUX_WANT_DB_POSTGRES)
    add_library(conflux_db STATIC)
    target_sources(conflux_db
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES
            src/db/types.cxx
            src/db/params.cxx
            src/db/result.cxx
            src/db/connection.cxx
            src/db/pool.cxx
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

# ---------------------------------------------------------------------------
# STDSIMD detection (before monolith, needed by conflux_json too)
# ---------------------------------------------------------------------------

if(CONFLUX_JSON_USE_STDSIMD STREQUAL "AUTO")
    include(CheckCXXSourceCompiles)
    set(_STDSIMD_TEST_CODE "
#include <experimental/simd>
#include <cstddef>
namespace stdx = std::experimental::parallelism_v2;
using vec_t = stdx::native_simd<signed char>;
int main() {
    vec_t a{};
    vec_t b{};
    auto mask = (a == b) | (a < b);
    if (stdx::any_of(mask))
        return static_cast<int>(stdx::find_first_set(mask));
    return static_cast<int>(vec_t::size());
}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fexperimental-library")
    endif()
    check_cxx_source_compiles("${_STDSIMD_TEST_CODE}" CONFLUX_HAS_STDSIMD)
    unset(CMAKE_REQUIRED_FLAGS)
    if(CONFLUX_HAS_STDSIMD)
        set(CONFLUX_JSON_USE_STDSIMD ON)
    else()
        set(CONFLUX_JSON_USE_STDSIMD OFF)
    endif()
endif()
if(CONFLUX_JSON_USE_STDSIMD AND TARGET conflux_json)
    message(STATUS "conflux: JSON stdsimd path enabled (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
    target_compile_definitions(conflux_json PRIVATE CONFLUX_JSON_USE_STDSIMD=1)
    target_sources(conflux_json PRIVATE src/json_simd_stdx.cxx)
    set_source_files_properties(src/json_simd_stdx.cxx PROPERTIES
        COMPILE_OPTIONS "-mavx2"
    )
endif()

add_library(conflux_utils STATIC)
target_sources(conflux_utils
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/utils.cxx
)
target_link_libraries(conflux_utils
    PRIVATE conflux_options
    PUBLIC  conflux_types
)
if(CONFLUX_JSON_USE_STDSIMD)
    target_compile_definitions(conflux_utils PRIVATE CONFLUX_STDSIMD=1)
    target_sources(conflux_utils PRIVATE src/simd_stdx.cxx)
    set_source_files_properties(src/simd_stdx.cxx PROPERTIES
        COMPILE_OPTIONS "-mavx2"
    )
endif()

add_library(conflux_net_config STATIC)
target_sources(conflux_net_config
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/config.cxx
)
target_link_libraries(conflux_net_config
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_utils
)

# Keep the package-facing core anchor aligned with the documented
# "types + utils" core surface while leaving HTTP configuration as a separate
# support component.
target_link_libraries(conflux_core INTERFACE conflux_utils)

if(CONFLUX_HAS_TLS STREQUAL "true"
        AND (CONFLUX_HTTP_ROUTER_STACK_REQUESTED OR CONFLUX_HTTP_CLIENT_STACK_REQUESTED
             OR CONFLUX_WANT_SMTP))
add_library(conflux_net_tls STATIC)
target_sources(conflux_net_tls
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/tls.cxx
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
    target_sources(conflux_json_reflect
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES src/json_reflect.cxx
    )
    target_link_libraries(conflux_json_reflect
        PUBLIC  conflux_json conflux_types
        PRIVATE conflux_options
    )
    message(STATUS "conflux: JSON P2996 reflection codec enabled (requires GCC 16+ -freflection)")
endif()

add_library(conflux_direct_slot_pool STATIC)
target_sources(conflux_direct_slot_pool
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/net/direct_slot_pool.cxx
)
target_link_libraries(conflux_direct_slot_pool
    PRIVATE conflux_types)

if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED)
add_library(conflux_http_router STATIC)
target_sources(conflux_http_router
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/router.cxx
)
target_sources(conflux_http_router
    PRIVATE
        src/net/router_impl.cxx
)
target_link_libraries(conflux_http_router
    PRIVATE conflux_options
    PUBLIC  conflux_http_core
    PUBLIC  conflux_crypto
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_utils
    PUBLIC  conflux_net_config
    PUBLIC  conflux_socket_io
)
if(TARGET conflux_net_tls)
    target_compile_definitions(conflux_http_router PUBLIC CONFLUX_HAS_TLS=1)
    target_link_libraries(conflux_http_router PUBLIC conflux_net_tls)
else()
    target_compile_definitions(conflux_http_router PUBLIC CONFLUX_HAS_TLS=0)
endif()
if(CONFLUX_JSON_USE_STDSIMD)
    target_compile_definitions(conflux_http_router PRIVATE CONFLUX_STDSIMD=1)
endif()
endif() # HTTP router or router-dependent component

if(CONFLUX_WANT_HTTP_POLICY OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_policy STATIC)
target_sources(conflux_http_policy
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        src/net/cache_control.cxx
        src/net/cors.cxx
        src/net/etag.cxx
        src/net/forwarded.cxx
        src/net/ip_filter.cxx
        src/net/rate_limit.cxx
        src/net/redirect.cxx
        src/net/response_cache.cxx
        src/net/security.cxx
        src/net/trailing_slash.cxx
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
    src/net/auth.cxx
    src/net/cookie_signing.cxx
    src/net/csrf.cxx
)
if(CONFLUX_HAS_TLS STREQUAL "true")
    list(APPEND CONFLUX_HTTP_AUTH_SOURCES src/net/jwt.cxx)
endif()
target_sources(conflux_http_auth
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${CONFLUX_HTTP_AUTH_SOURCES}
)
target_link_libraries(conflux_http_auth
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
    PUBLIC  conflux_crypto
    PUBLIC  conflux_utils
)
if(CONFLUX_HAS_TLS STREQUAL "true")
    target_compile_definitions(conflux_http_auth PUBLIC CONFLUX_HAS_TLS=1)
else()
    target_compile_definitions(conflux_http_auth PUBLIC CONFLUX_HAS_TLS=0)
endif()
endif() # CONFLUX_WANT_HTTP_AUTH || CONFLUX_WANT_HTTP_SERVER

if(CONFLUX_WANT_HTTP_OBSERVABILITY OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_observability STATIC)
set(CONFLUX_HTTP_OBSERVABILITY_SOURCES
    src/net/request_id.cxx
    src/net/structured_log.cxx
    src/net/tracing.cxx
)
if(CONFLUX_HAS_METRICS STREQUAL "true")
    list(APPEND CONFLUX_HTTP_OBSERVABILITY_SOURCES src/net/metrics.cxx)
endif()
target_sources(conflux_http_observability
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${CONFLUX_HTTP_OBSERVABILITY_SOURCES}
)
target_link_libraries(conflux_http_observability
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
    PUBLIC  conflux_utils
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
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/openapi.cxx
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
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/vhost.cxx
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
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/client_dns_bridge.cxx
)
target_link_libraries(conflux_dns_bridge
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_dns
)

add_library(conflux_net_client STATIC)
target_sources(conflux_net_client
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/client.cxx
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
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/client_async.cxx
)
target_sources(conflux_net_async_client
    PRIVATE
        src/net/client_async_impl.cxx
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
set(CONFLUX_HTTP_COMPRESSION_SOURCES src/net/compress.cxx)
if(CONFLUX_HAS_ZLIB STREQUAL "true")
    list(APPEND CONFLUX_HTTP_COMPRESSION_SOURCES src/net/compress_backend_zlib.cxx)
endif()
if(CONFLUX_HAS_LIBDEFLATE STREQUAL "true")
    list(APPEND CONFLUX_HTTP_COMPRESSION_SOURCES src/net/compress_backend_libdeflate.cxx)
endif()
if(CONFLUX_HAS_ZLIB_NG STREQUAL "true")
    list(APPEND CONFLUX_HTTP_COMPRESSION_SOURCES src/net/compress_backend_zlibng.cxx)
endif()
if(CONFLUX_HAS_ISAL STREQUAL "true")
    list(APPEND CONFLUX_HTTP_COMPRESSION_SOURCES src/net/compress_backend_isal.cxx)
endif()
target_sources(conflux_http_compression
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
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
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/proxy.cxx
)
target_sources(conflux_http_proxy
    PRIVATE
        src/net/proxy_impl.cxx
)
target_link_libraries(conflux_http_proxy
    PRIVATE conflux_options
    PUBLIC  conflux_http_router
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
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES src/net/smtp.cxx
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

# ---------------------------------------------------------------------------
# Monolith target (HTTP server + all middleware)
# ---------------------------------------------------------------------------

if(CONFLUX_WANT_HTTP_SERVER)

add_library(conflux STATIC)

# Core module sources.
set(CONFLUX_MODULE_SOURCES
    src/process.cxx
    src/net/io_buffer.cxx
    src/conflux.cxx
    src/net/app.cxx
    src/net/http.cxx
    src/net/http1_parser.cxx
    src/net/http_server.cxx
)

# Optional module sources.
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    list(APPEND CONFLUX_MODULE_SOURCES src/net/http2.cxx)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    list(APPEND CONFLUX_MODULE_SOURCES src/net/http3.cxx)
endif()

target_sources(conflux
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        ${CONFLUX_MODULE_SOURCES}
)

target_sources(conflux
    PRIVATE
        src/net/http_server_impl.cxx
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
    PUBLIC  conflux_http_router
    PUBLIC  conflux_http_policy
    PUBLIC  conflux_http_auth
    PUBLIC  conflux_http_observability
    PUBLIC  conflux_http_openapi
    PUBLIC  conflux_http_vhost
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_socket_io
    PRIVATE conflux_direct_slot_pool
    PUBLIC  PkgConfig::LIBURING
)
if(TARGET conflux_template)
    target_link_libraries(conflux PUBLIC conflux_template)
endif()
if(TARGET conflux_file_watch)
    target_link_libraries(conflux PUBLIC conflux_file_watch)
endif()
if(TARGET conflux_json)
    target_link_libraries(conflux PUBLIC conflux_json)
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
        CONFLUX_HAS_JSON_FILE=$<BOOL:${CONFLUX_WANT_JSON_FILE}>
        CONFLUX_HAS_TEMPLATES=$<BOOL:${CONFLUX_WANT_TEMPLATES}>
        CONFLUX_HAS_FILE_WATCH=$<BOOL:${CONFLUX_WANT_FILE_WATCH}>
)

# TLS
if(CONFLUX_HAS_TLS STREQUAL "true")
    target_compile_definitions(conflux PUBLIC CONFLUX_HAS_TLS=1)
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
# Tests & examples (monolith-dependent)
# ---------------------------------------------------------------------------

if(CONFLUX_BUILD_TESTS)
    enable_testing()
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

add_executable(conflux_dual examples/dual.cxx)
target_link_libraries(conflux_dual PRIVATE conflux conflux_options)

if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    add_executable(conflux_h3_probe examples/h3_probe.cxx)
    target_link_libraries(conflux_h3_probe PRIVATE conflux conflux_options)
    add_executable(conflux_h3_server examples/h3_server.cxx)
    target_link_libraries(conflux_h3_server PRIVATE conflux conflux_options)
endif()

if(CONFLUX_HAS_DB STREQUAL "true" AND TARGET conflux_db)
    add_executable(conflux_db_basic examples/db_basic.cxx)
    target_link_libraries(conflux_db_basic
        PRIVATE conflux_db conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)

    add_executable(conflux_db_pool examples/db_pool.cxx)
    target_link_libraries(conflux_db_pool
        PRIVATE conflux_db conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)
endif()

# JSON examples (use monolith for convenience)
add_executable(conflux_json_example examples/json.cxx)
target_link_libraries(conflux_json_example PRIVATE conflux conflux_options)

endif() # CONFLUX_WANT_HTTP_SERVER

# ---------------------------------------------------------------------------
# Public package targets / install-export
# ---------------------------------------------------------------------------

function(conflux_public_component target export_name)
    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
        if(NOT TARGET conflux::${export_name})
            add_library(conflux::${export_name} ALIAS ${target})
        endif()
    endif()
endfunction()

conflux_public_component(conflux_core core)
conflux_public_component(conflux_types types)
conflux_public_component(conflux_utils utils)
conflux_public_component(conflux_net_config net_config)
conflux_public_component(conflux_net_cancel net_cancel)
conflux_public_component(conflux_net_tls net_tls)
conflux_public_component(conflux_crypto crypto)
conflux_public_component(conflux_json json)
conflux_public_component(conflux_json_file json_file)
conflux_public_component(conflux_json_reflect json_reflect)
conflux_public_component(conflux_template template)
conflux_public_component(conflux_file_watch template_watch)
conflux_public_component(conflux_http_core http_core)
conflux_public_component(conflux_http_router http_router)
conflux_public_component(conflux_http_policy http_policy)
conflux_public_component(conflux_http_auth http_auth)
conflux_public_component(conflux_http_compression http_compression)
conflux_public_component(conflux_net_client http_client)
conflux_public_component(conflux_net_async_client http_async_client)
conflux_public_component(conflux_http_proxy http_proxy)
conflux_public_component(conflux_net_smtp smtp)
conflux_public_component(conflux_dns_bridge _dns_bridge)
conflux_public_component(conflux_http_observability http_observability)
conflux_public_component(conflux_http_openapi http_openapi)
conflux_public_component(conflux_http_vhost http_vhost)
conflux_public_component(conflux_http_json http_json)
conflux_public_component(conflux_uring uring)
conflux_public_component(conflux_work runtime)
conflux_public_component(conflux_file_io_sync file_io_sync)
conflux_public_component(conflux_file_map file_map)
conflux_public_component(conflux_file_io file_io)
conflux_public_component(conflux_socket_io socket_io)
conflux_public_component(conflux_dns dns)
conflux_public_component(conflux_db db)
conflux_public_component(conflux http_server)

# Internal-but-exported support targets needed by static-library link interfaces.
conflux_public_component(conflux_options _options)
conflux_public_component(conflux_direct_slot_pool _direct_slot_pool)

set(CONFLUX_INSTALL_INTERFACE_TARGETS)
foreach(_target IN ITEMS
        conflux_options
        conflux_core)
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
        conflux_crypto
        conflux_json
        conflux_json_file
        conflux_json_reflect
        conflux_template
        conflux_file_watch
        conflux_http_core
        conflux_http_router
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
        conflux_uring
        conflux_work
        conflux_file_io_sync
        conflux_file_map
        conflux_file_io
        conflux_socket_io
        conflux_dns
        conflux_db
        conflux_direct_slot_pool)
    if(TARGET ${_target})
        list(APPEND CONFLUX_INSTALL_MODULE_TARGETS ${_target})
    endif()
endforeach()

if(CONFLUX_INSTALL_INTERFACE_TARGETS)
    install(TARGETS ${CONFLUX_INSTALL_INTERFACE_TARGETS}
        EXPORT confluxTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(CONFLUX_INSTALL_MODULE_TARGETS)
    install(TARGETS ${CONFLUX_INSTALL_MODULE_TARGETS}
        EXPORT confluxTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules
    )
endif()

if(TARGET conflux)
    install(TARGETS conflux
        EXPORT confluxTargets
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

if(TARGET conflux_uring OR TARGET conflux_work OR TARGET conflux_file_io OR TARGET conflux_file_watch OR TARGET conflux_socket_io OR TARGET conflux_dns OR TARGET conflux_db OR TARGET conflux)
    set(CONFLUX_INSTALL_NEEDS_LIBURING TRUE)
endif()
if(TARGET conflux_json)
    set(CONFLUX_INSTALL_NEEDS_XXHASH TRUE)
endif()
if(TARGET conflux_db)
    set(CONFLUX_INSTALL_NEEDS_LIBPQ TRUE)
endif()
if(TARGET conflux_net_tls OR (TARGET conflux AND CONFLUX_HAS_TLS STREQUAL "true"))
    set(CONFLUX_INSTALL_NEEDS_OPENSSL TRUE)
endif()
if((TARGET conflux OR TARGET conflux_http_compression) AND CONFLUX_HAS_ZLIB STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_ZLIB TRUE)
endif()
if((TARGET conflux OR TARGET conflux_http_compression) AND CONFLUX_HAS_LIBDEFLATE STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_LIBDEFLATE TRUE)
endif()
if((TARGET conflux OR TARGET conflux_http_compression) AND CONFLUX_HAS_ZLIB_NG STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_ZLIB_NG TRUE)
endif()
if((TARGET conflux OR TARGET conflux_http_compression) AND CONFLUX_HAS_ISAL STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_ISAL TRUE)
endif()
if((TARGET conflux OR TARGET conflux_http_compression) AND CONFLUX_HAS_BROTLI STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_BROTLI TRUE)
endif()
if((TARGET conflux OR TARGET conflux_http_compression) AND CONFLUX_HAS_ZSTD STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_ZSTD TRUE)
endif()
if(TARGET conflux AND CONFLUX_HAS_HTTP2 STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_NGHTTP2 TRUE)
endif()
if(TARGET conflux AND CONFLUX_HAS_HTTP3 STREQUAL "true")
    set(CONFLUX_INSTALL_NEEDS_NGTCP2 TRUE)
    set(CONFLUX_INSTALL_NEEDS_NGHTTP3 TRUE)
endif()

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/conflux-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
)
install(EXPORT confluxTargets
    NAMESPACE conflux::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
)

# ---------------------------------------------------------------------------
# Standalone examples (don't require monolith)
# ---------------------------------------------------------------------------

if(CONFLUX_WANT_FILE_IO)
    add_executable(conflux_file_io_example examples/file_io.cxx)
    target_link_libraries(conflux_file_io_example PRIVATE conflux_file_io conflux_work conflux_options)

    add_executable(conflux_coroutines examples/coroutines.cxx)
    target_link_libraries(conflux_coroutines PRIVATE conflux_file_io conflux_work conflux_options)
endif()

if(CONFLUX_JSON_REFLECT AND TARGET conflux_json_reflect)
    add_executable(conflux_json_reflect_example examples/json_reflect.cxx)
    target_link_libraries(conflux_json_reflect_example
        PRIVATE conflux_json_reflect conflux_json conflux_types conflux_options)
endif()
