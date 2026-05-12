# cmake/Dependencies.cmake
# Resolved in order: system packages first, then optional FetchContent for
# small test-only dependencies not yet packaged on this host.

find_package(PkgConfig REQUIRED)

if(CONFLUX_BUILD_TESTS)
    find_package(Catch2 3 QUIET)
    if(Catch2_FOUND)
        message(STATUS "conflux: tests using system Catch2")
    elseif(CONFLUX_FETCH_TEST_DEPS)
        include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.12.0
            GIT_SHALLOW    TRUE
            OVERRIDE_FIND_PACKAGE
        )
        FetchContent_MakeAvailable(Catch2)
    else()
        message(FATAL_ERROR
            "conflux: CONFLUX_BUILD_TESTS=ON requires Catch2 3. "
            "Install Catch2 and/or pass -DCMAKE_PREFIX_PATH=<prefix>, "
            "enable -DCONFLUX_FETCH_TEST_DEPS=ON, or disable tests with "
            "-DCONFLUX_BUILD_TESTS=OFF.")
    endif()

    set(CONFLUX_HAS_JSON_TESTSUITE FALSE)
    if(CONFLUX_ENABLE_JSON_TESTSUITE)
        if(JSONTESTSUITE_DIR AND EXISTS "${JSONTESTSUITE_DIR}/test_parsing")
            set(JSONTESTSUITE_DIR "${JSONTESTSUITE_DIR}/test_parsing" CACHE PATH
                "Path to nst/JSONTestSuite/test_parsing" FORCE)
        endif()

        if(JSONTESTSUITE_DIR AND EXISTS "${JSONTESTSUITE_DIR}")
            set(CONFLUX_HAS_JSON_TESTSUITE TRUE)
            message(STATUS "conflux: JSONTestSuite gate enabled (${JSONTESTSUITE_DIR})")
        elseif(CONFLUX_FETCH_TEST_DEPS)
            include(FetchContent)
            FetchContent_Declare(
                JSONTestSuite
                GIT_REPOSITORY https://github.com/nst/JSONTestSuite.git
                GIT_TAG        master
                GIT_SHALLOW    TRUE
            )
            FetchContent_MakeAvailable(JSONTestSuite)
            set(JSONTESTSUITE_DIR "${jsontestsuite_SOURCE_DIR}/test_parsing" CACHE PATH
                "Path to nst/JSONTestSuite/test_parsing" FORCE)
            set(CONFLUX_HAS_JSON_TESTSUITE TRUE)
            message(STATUS "conflux: JSONTestSuite gate enabled (${JSONTESTSUITE_DIR})")
        else()
            message(STATUS
                "conflux: JSONTestSuite gate skipped "
                "(CONFLUX_ENABLE_JSON_TESTSUITE=ON, but JSONTESTSUITE_DIR is unset "
                "and CONFLUX_FETCH_TEST_DEPS=OFF)")
        endif()
    else()
        message(STATUS "conflux: JSONTestSuite gate disabled")
    endif()
endif()
if(CONFLUX_NEEDS_RUNTIME)
    pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)
else()
    message(STATUS "conflux: liburing not required by preset '${CONFLUX_FEATURE_SET}'")
endif()
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
