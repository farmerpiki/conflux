# cmake/Dependencies.cmake
# Resolved in order: selected system packages first, then optional FetchContent
# for small test-only dependencies not yet packaged on this host. pkg-config is
# requested lazily so core/header/package configures do not require it.

function(conflux_require_pkg_config reason)
    if(NOT PkgConfig_FOUND)
        find_package(PkgConfig REQUIRED)
    endif()
endfunction()

function(conflux_pkg_provider prefix required)
    set(_packages ${ARGN})
    if(NOT _packages)
        message(FATAL_ERROR "conflux_pkg_provider(${prefix}): missing pkg-config package")
    endif()
    conflux_require_pkg_config("${prefix}")
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --exists ${_packages}
        RESULT_VARIABLE _exists
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT _exists EQUAL 0)
        set(${prefix}_FOUND FALSE PARENT_SCOPE)
        if(required)
            string(JOIN " " _pkg_list ${_packages})
            message(FATAL_ERROR "conflux: required pkg-config package(s) missing for ${prefix}: ${_pkg_list}")
        endif()
        return()
    endif()

    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --modversion ${ARGV2}
        OUTPUT_VARIABLE _version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --cflags ${_packages}
        OUTPUT_VARIABLE _cflags
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --libs ${_packages}
        OUTPUT_VARIABLE _libs
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if(NOT TARGET PkgConfig::${prefix})
        add_library(PkgConfig::${prefix} INTERFACE IMPORTED)
        if(NOT _cflags STREQUAL "")
            separate_arguments(_cflags_list UNIX_COMMAND "${_cflags}")
            set_target_properties(PkgConfig::${prefix} PROPERTIES
                INTERFACE_COMPILE_OPTIONS "${_cflags_list}")
        endif()
        if(NOT _libs STREQUAL "")
            separate_arguments(_libs_list UNIX_COMMAND "${_libs}")
            set_target_properties(PkgConfig::${prefix} PROPERTIES
                INTERFACE_LINK_LIBRARIES "${_libs_list}")
        endif()
    endif()
    set(${prefix}_FOUND TRUE PARENT_SCOPE)
    set(${prefix}_VERSION "${_version}" PARENT_SCOPE)
endfunction()

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
    conflux_require_pkg_config("liburing")
    conflux_pkg_provider(LIBURING TRUE liburing)
else()
    message(STATUS "conflux: liburing not required by preset '${CONFLUX_FEATURE_SET}'")
endif()

string(TOUPPER "${CONFLUX_JSON_HASH_PROVIDER}" CONFLUX_JSON_HASH_PROVIDER_UPPER)
if(NOT CONFLUX_JSON_HASH_PROVIDER_UPPER MATCHES "^(AUTO|XXHASH|INTERNAL)$")
    message(FATAL_ERROR
        "conflux: CONFLUX_JSON_HASH_PROVIDER must be AUTO, XXHASH, or INTERNAL "
        "(got '${CONFLUX_JSON_HASH_PROVIDER}')")
endif()
if(CONFLUX_WANT_JSON AND NOT CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "INTERNAL")
    if(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "XXHASH")
        conflux_require_pkg_config("libxxhash")
        conflux_pkg_provider(XXHASH TRUE libxxhash)
    else()
        conflux_require_pkg_config("libxxhash")
        conflux_pkg_provider(XXHASH FALSE libxxhash)
        if(XXHASH_FOUND)
            set(CONFLUX_JSON_HASH_PROVIDER_UPPER "XXHASH")
        else()
            set(CONFLUX_JSON_HASH_PROVIDER_UPPER "INTERNAL")
        endif()
        set(CONFLUX_JSON_HASH_PROVIDER_UPPER "${CONFLUX_JSON_HASH_PROVIDER_UPPER}" CACHE INTERNAL
            "Resolved JSON hash provider" FORCE)
    endif()
elseif(CONFLUX_WANT_JSON)
    set(CONFLUX_JSON_HASH_PROVIDER_UPPER "INTERNAL" CACHE INTERNAL
        "Resolved JSON hash provider" FORCE)
endif()
if(CONFLUX_WANT_JSON)
    message(STATUS "conflux: JSON hash provider ${CONFLUX_JSON_HASH_PROVIDER_UPPER}")
endif()

if(CONFLUX_WANT_HTTP_COMPRESSION)
    conflux_pkg_provider(BROTLI FALSE libbrotlienc libbrotlidec)
    conflux_pkg_provider(ZSTD FALSE libzstd)
    conflux_pkg_provider(LIBDEFLATE FALSE libdeflate)
    conflux_pkg_provider(ZLIB_NG FALSE zlib-ng)
    conflux_pkg_provider(LIBISAL FALSE libisal)
endif()
if(CONFLUX_WANT_HTTP_SERVER OR CONFLUX_WANT_HTTP_CLIENT)
    conflux_pkg_provider(NGHTTP2 FALSE libnghttp2)
endif()
if(CONFLUX_WANT_HTTP_SERVER AND CONFLUX_ENABLE_EXPERIMENTAL AND CONFLUX_ENABLE_HTTP3)
    conflux_pkg_provider(NGTCP2 FALSE libngtcp2)
    conflux_pkg_provider(NGTCP2_CRYPTO_OSSL FALSE libngtcp2_crypto_ossl)
    conflux_pkg_provider(NGHTTP3 FALSE libnghttp3)
endif()
string(TOUPPER "${CONFLUX_PASSWORD_HASH_ARGON2_PROVIDER}" CONFLUX_ARGON2_PROVIDER_UPPER)
if(CONFLUX_WANT_HTTP_AUTH
        AND (CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "AUTO"
            OR CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "SYSTEM"))
    conflux_pkg_provider(ARGON2 FALSE libargon2)
endif()
if(CONFLUX_WANT_DB_POSTGRES)
    conflux_pkg_provider(LIBPQ FALSE libpq)
endif()
