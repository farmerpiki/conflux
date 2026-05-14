# cmake/Dependencies.cmake
# Resolved in order: system packages first, then optional FetchContent for
# small test-only dependencies not yet packaged on this host.

find_package(PkgConfig REQUIRED)

if(CONFLUX_BUILD_TESTS)
    string(TOUPPER "${CONFLUX_TEST_CATCH2_PROVIDER}" CONFLUX_TEST_CATCH2_PROVIDER_UPPER)
    if(NOT CONFLUX_TEST_CATCH2_PROVIDER_UPPER MATCHES "^(FETCH|SYSTEM|AUTO)$")
        message(FATAL_ERROR
            "conflux: CONFLUX_TEST_CATCH2_PROVIDER must be FETCH, SYSTEM, or AUTO "
            "(got '${CONFLUX_TEST_CATCH2_PROVIDER}')")
    endif()

    if(CONFLUX_CATCH2_SOURCE_DIR)
        set(FETCHCONTENT_SOURCE_DIR_CATCH2 "${CONFLUX_CATCH2_SOURCE_DIR}" CACHE PATH
            "Local Catch2 source tree" FORCE)
    endif()

    set(_conflux_catch2_local_source FALSE)
    if(DEFINED FETCHCONTENT_SOURCE_DIR_CATCH2 AND NOT "${FETCHCONTENT_SOURCE_DIR_CATCH2}" STREQUAL "")
        set(_conflux_catch2_local_source TRUE)
    endif()

    function(conflux_fetch_catch2)
        if(NOT CONFLUX_FETCH_TEST_DEPS AND NOT _conflux_catch2_local_source)
            message(FATAL_ERROR
                "conflux: Catch2 provider FETCH needs either "
                "-DCONFLUX_FETCH_TEST_DEPS=ON to download Catch2, "
                "-DCONFLUX_CATCH2_SOURCE_DIR=<local Catch2 source dir>, or "
                "-DFETCHCONTENT_SOURCE_DIR_CATCH2=<local Catch2 source dir>. "
                "Alternatively use -DCONFLUX_TEST_CATCH2_PROVIDER=SYSTEM with a compatible Catch2 package.")
        endif()

        include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.12.0
            GIT_SHALLOW    TRUE
            OVERRIDE_FIND_PACKAGE
        )
        FetchContent_MakeAvailable(Catch2)
        FetchContent_GetProperties(Catch2 SOURCE_DIR _conflux_catch2_source_dir)
        set(CONFLUX_CATCH2_EXTRAS_DIR "${_conflux_catch2_source_dir}/extras" PARENT_SCOPE)
        message(STATUS "conflux: tests using source-built Catch2")
    endfunction()

    if(CONFLUX_TEST_CATCH2_PROVIDER_UPPER STREQUAL "SYSTEM")
        find_package(Catch2 3 REQUIRED)
        message(STATUS "conflux: tests using system Catch2")
    elseif(CONFLUX_TEST_CATCH2_PROVIDER_UPPER STREQUAL "AUTO")
        find_package(Catch2 3 QUIET)
        if(Catch2_FOUND)
            message(STATUS "conflux: tests using system Catch2")
        else()
            conflux_fetch_catch2()
        endif()
    else()
        conflux_fetch_catch2()
    endif()

    if(NOT CONFLUX_CATCH2_EXTRAS_DIR AND DEFINED Catch2_DIR)
        set(CONFLUX_CATCH2_EXTRAS_DIR "${Catch2_DIR}")
    endif()
    if(CONFLUX_CATCH2_EXTRAS_DIR AND EXISTS "${CONFLUX_CATCH2_EXTRAS_DIR}/Catch.cmake")
        list(APPEND CMAKE_MODULE_PATH "${CONFLUX_CATCH2_EXTRAS_DIR}")
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
if(CONFLUX_WANT_JSON)
    pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
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
pkg_check_modules(ARGON2    QUIET IMPORTED_TARGET libargon2)
pkg_check_modules(LIBPQ     IMPORTED_TARGET libpq)
