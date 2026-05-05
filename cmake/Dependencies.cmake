# cmake/Dependencies.cmake
# Resolved in order: system find_package, then FetchContent for small deps
# not yet packaged on this host.

find_package(PkgConfig REQUIRED)

# Catch2 — always built from source so it matches the active stdlib (libc++ vs libstdc++).
if(CONFLUX_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.12.0
        GIT_SHALLOW    TRUE
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(Catch2)

    FetchContent_Declare(
        JSONTestSuite
        GIT_REPOSITORY https://github.com/nst/JSONTestSuite.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JSONTestSuite)
    set(JSONTESTSUITE_DIR "${jsontestsuite_SOURCE_DIR}/test_parsing" CACHE INTERNAL "")
endif()
pkg_check_modules(LIBURING  REQUIRED IMPORTED_TARGET liburing)
pkg_check_modules(BROTLI    IMPORTED_TARGET libbrotlienc libbrotlidec)
pkg_check_modules(ZSTD      IMPORTED_TARGET libzstd)
pkg_check_modules(LIBDEFLATE IMPORTED_TARGET libdeflate)
pkg_check_modules(ZLIB_NG   IMPORTED_TARGET zlib-ng)
pkg_check_modules(LIBISAL   IMPORTED_TARGET libisal)
pkg_check_modules(NGHTTP2   IMPORTED_TARGET libnghttp2)
pkg_check_modules(NGTCP2    IMPORTED_TARGET libngtcp2)
pkg_check_modules(NGTCP2_CRYPTO_OSSL IMPORTED_TARGET libngtcp2_crypto_ossl)
pkg_check_modules(NGHTTP3   IMPORTED_TARGET libnghttp3)
pkg_check_modules(LIBPQ     IMPORTED_TARGET libpq)
