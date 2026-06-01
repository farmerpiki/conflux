# cmake/Dependencies.cmake
# Resolved in order: selected system packages first, then optional FetchContent
# for small test-only dependencies not yet packaged on this host. pkg-config is
# requested lazily so core/header/package configures do not require it.

function(conflux_require_pkg_config reason)
    if(NOT PkgConfig_FOUND)
        find_package(PkgConfig REQUIRED)
    endif()
endfunction()

function(conflux_find_pkg_config out required reason)
    if(PkgConfig_FOUND AND PKG_CONFIG_EXECUTABLE)
        set(${out} TRUE PARENT_SCOPE)
        return()
    endif()

    if(required)
        find_package(PkgConfig REQUIRED)
    else()
        find_package(PkgConfig QUIET)
    endif()

    if(PkgConfig_FOUND AND PKG_CONFIG_EXECUTABLE)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(conflux_pkg_provider prefix required)
    set(_packages ${ARGN})
    if(NOT _packages)
        message(FATAL_ERROR "conflux_pkg_provider(${prefix}): missing pkg-config package")
    endif()
    conflux_find_pkg_config(_conflux_has_pkg_config "${required}" "${prefix}")
    if(NOT _conflux_has_pkg_config)
        set(${prefix}_FOUND FALSE PARENT_SCOPE)
        if(required)
            message(FATAL_ERROR "conflux: required pkg-config executable missing for ${prefix}")
        endif()
        return()
    endif()
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
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --cflags-only-I ${_packages}
        OUTPUT_VARIABLE _include_flags
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --libs ${_packages}
        OUTPUT_VARIABLE _libs
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if(NOT TARGET PkgConfig::${prefix})
        add_library(PkgConfig::${prefix} INTERFACE IMPORTED)
        set(_include_dirs)
        if(NOT _include_flags STREQUAL "")
            separate_arguments(_include_flags_list UNIX_COMMAND "${_include_flags}")
            foreach(_flag IN LISTS _include_flags_list)
                if(_flag MATCHES "^-I(.+)$")
                    list(APPEND _include_dirs "${CMAKE_MATCH_1}")
                endif()
            endforeach()
            if(_include_dirs)
                set_target_properties(PkgConfig::${prefix} PROPERTIES
                    INTERFACE_INCLUDE_DIRECTORIES "${_include_dirs}")
            endif()
        endif()
        if(NOT _cflags STREQUAL "")
            separate_arguments(_cflags_list UNIX_COMMAND "${_cflags}")
            foreach(_dir IN LISTS _include_dirs)
                list(REMOVE_ITEM _cflags_list "-I${_dir}")
            endforeach()
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

function(conflux_gzip_probe_source backend out)
    if(backend STREQUAL "LIBDEFLATE")
        set(_code [[
#include <chrono>
#include <cstdio>
#include <string>
#include <libdeflate.h>
int main() {
    std::string input;
    for (int i = 0; i < 256; ++i) input += "content-type: application/json\n{\"hello\":\"world\",\"value\":123456789}\n";
    auto *c = libdeflate_alloc_compressor(6);
    if (!c) return 2;
    auto bound = libdeflate_gzip_compress_bound(c, input.size());
    std::string out(bound, '\0');
    auto start = std::chrono::steady_clock::now();
    std::size_t total = 0;
    for (int i = 0; i < 256; ++i) {
        auto n = libdeflate_gzip_compress(c, input.data(), input.size(), out.data(), out.size());
        if (n == 0) return 3;
        total += n;
    }
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("%lld %zu\n", static_cast<long long>(ns), total);
    libdeflate_free_compressor(c);
}
]])
    elseif(backend STREQUAL "ZLIB_NG")
        set(_code [[
#include <chrono>
#include <cstdio>
#include <string>
#include <zlib-ng.h>
int main() {
    std::string input;
    for (int i = 0; i < 256; ++i) input += "content-type: application/json\n{\"hello\":\"world\",\"value\":123456789}\n";
    std::string out(input.size() + input.size() / 8 + 256, '\0');
    auto start = std::chrono::steady_clock::now();
    std::size_t total = 0;
    for (int i = 0; i < 256; ++i) {
        zng_stream zs{};
        if (zng_deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 2;
        zs.next_in = reinterpret_cast<unsigned char const *>(input.data());
        zs.avail_in = static_cast<unsigned int>(input.size());
        zs.next_out = reinterpret_cast<unsigned char *>(out.data());
        zs.avail_out = static_cast<unsigned int>(out.size());
        int rc = zng_deflate(&zs, Z_FINISH);
        zng_deflateEnd(&zs);
        if (rc != Z_STREAM_END) return 3;
        total += zs.total_out;
    }
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("%lld %zu\n", static_cast<long long>(ns), total);
}
]])
    elseif(backend STREQUAL "ISAL")
        set(_code [[
#include <chrono>
#include <cstdio>
#include <string>
#include <isa-l/igzip_lib.h>
int main() {
    std::string input;
    for (int i = 0; i < 256; ++i) input += "content-type: application/json\n{\"hello\":\"world\",\"value\":123456789}\n";
    std::string out(input.size() + input.size() / 8 + 256, '\0');
    auto start = std::chrono::steady_clock::now();
    std::size_t total = 0;
    for (int i = 0; i < 256; ++i) {
        isal_zstream stream{};
        isal_deflate_stateless_init(&stream);
        stream.next_in = reinterpret_cast<unsigned char *>(const_cast<char *>(input.data()));
        stream.avail_in = static_cast<unsigned int>(input.size());
        stream.next_out = reinterpret_cast<unsigned char *>(out.data());
        stream.avail_out = static_cast<unsigned int>(out.size());
        stream.level = 1;
        stream.end_of_stream = 1;
        stream.flush = FULL_FLUSH;
        stream.gzip_flag = IGZIP_GZIP;
        if (isal_deflate_stateless(&stream) != COMP_OK) return 2;
        total += stream.total_out;
    }
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("%lld %zu\n", static_cast<long long>(ns), total);
}
]])
    elseif(backend STREQUAL "ZLIB")
        set(_code [[
#include <chrono>
#include <cstdio>
#include <string>
#include <zlib.h>
int main() {
    std::string input;
    for (int i = 0; i < 256; ++i) input += "content-type: application/json\n{\"hello\":\"world\",\"value\":123456789}\n";
    std::string out(input.size() + input.size() / 8 + 256, '\0');
    auto start = std::chrono::steady_clock::now();
    std::size_t total = 0;
    for (int i = 0; i < 256; ++i) {
        z_stream zs{};
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 2;
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
        zs.avail_in = static_cast<uInt>(input.size());
        zs.next_out = reinterpret_cast<Bytef *>(out.data());
        zs.avail_out = static_cast<uInt>(out.size());
        int rc = deflate(&zs, Z_FINISH);
        deflateEnd(&zs);
        if (rc != Z_STREAM_END) return 3;
        total += zs.total_out;
    }
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("%lld %zu\n", static_cast<long long>(ns), total);
}
]])
    else()
        message(FATAL_ERROR "unknown gzip probe backend '${backend}'")
    endif()
    set(${out} "${_code}" PARENT_SCOPE)
endfunction()

function(conflux_benchmark_gzip_backend backend out_ns)
    conflux_gzip_probe_source("${backend}" _source)
    set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/conflux-provider-probes")
    file(MAKE_DIRECTORY "${_dir}")
    set(_src "${_dir}/gzip_${backend}.cxx")
    set(_exe "${_dir}/gzip_${backend}")
    file(WRITE "${_src}" "${_source}")
    if(backend STREQUAL "LIBDEFLATE")
        set(_target PkgConfig::LIBDEFLATE)
    elseif(backend STREQUAL "ZLIB_NG")
        set(_target PkgConfig::ZLIB_NG)
    elseif(backend STREQUAL "ISAL")
        set(_target PkgConfig::LIBISAL)
    elseif(backend STREQUAL "ZLIB")
        set(_target PkgConfig::ZLIB_PKG)
    else()
        message(FATAL_ERROR "unknown gzip probe backend '${backend}'")
    endif()
    get_target_property(_libs ${_target} INTERFACE_LINK_LIBRARIES)
    get_target_property(_copts ${_target} INTERFACE_COMPILE_OPTIONS)
    if(NOT _libs)
        set(_libs "")
    endif()
    if(NOT _copts)
        set(_copts "")
    endif()
    set(_compiler_flags)
    if(NOT CMAKE_CXX_FLAGS STREQUAL "")
        separate_arguments(_compiler_flags UNIX_COMMAND "${CMAKE_CXX_FLAGS}")
    endif()
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type_upper)
    if(_build_type_upper AND NOT CMAKE_CXX_FLAGS_${_build_type_upper} STREQUAL "")
        separate_arguments(_build_type_flags UNIX_COMMAND "${CMAKE_CXX_FLAGS_${_build_type_upper}}")
        list(APPEND _compiler_flags ${_build_type_flags})
    endif()
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" ${_compiler_flags} -O2 -std=c++17 ${_copts} "${_src}" -o "${_exe}" ${_libs}
        RESULT_VARIABLE _compile_rc
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT _compile_rc EQUAL 0)
        set(${out_ns} "" PARENT_SCOPE)
        return()
    endif()
    execute_process(
        COMMAND "${_exe}"
        RESULT_VARIABLE _run_rc
        OUTPUT_VARIABLE _run_out
        ERROR_QUIET
        TIMEOUT 5)
    if(NOT _run_rc EQUAL 0)
        set(${out_ns} "" PARENT_SCOPE)
        return()
    endif()
    string(REGEX MATCH "^[0-9]+" _ns "${_run_out}")
    set(${out_ns} "${_ns}" PARENT_SCOPE)
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
        conflux_pkg_provider(XXHASH TRUE libxxhash)
    else()
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
    set(CONFLUX_RESOLVED_JSON_HASH_PROVIDER "${CONFLUX_JSON_HASH_PROVIDER_UPPER}" CACHE STRING
        "Resolved JSON object-name hash provider" FORCE)
    message(STATUS "conflux: JSON hash provider ${CONFLUX_JSON_HASH_PROVIDER_UPPER}")
else()
    set(CONFLUX_RESOLVED_JSON_HASH_PROVIDER "INTERNAL" CACHE STRING
        "Resolved JSON object-name hash provider" FORCE)
endif()

string(TOUPPER "${CONFLUX_GZIP_PROVIDER}" CONFLUX_GZIP_PROVIDER_UPPER)
string(TOUPPER "${CONFLUX_BROTLI_PROVIDER}" CONFLUX_BROTLI_PROVIDER_UPPER)
string(TOUPPER "${CONFLUX_ZSTD_PROVIDER}" CONFLUX_ZSTD_PROVIDER_UPPER)
if(CONFLUX_WANT_HTTP_COMPRESSION AND NOT CONFLUX_GZIP_PROVIDER_UPPER STREQUAL "OFF")
    conflux_pkg_provider(LIBDEFLATE FALSE libdeflate)
    conflux_pkg_provider(ZLIB_NG FALSE zlib-ng)
    conflux_pkg_provider(LIBISAL FALSE libisal)
endif()
if(CONFLUX_WANT_HTTP_COMPRESSION AND NOT CONFLUX_BROTLI_PROVIDER_UPPER STREQUAL "OFF")
    conflux_pkg_provider(BROTLI FALSE libbrotlienc libbrotlidec)
endif()
if(CONFLUX_WANT_HTTP_COMPRESSION AND NOT CONFLUX_ZSTD_PROVIDER_UPPER STREQUAL "OFF")
    conflux_pkg_provider(ZSTD FALSE libzstd)
endif()
string(TOUPPER "${CONFLUX_HTTP2_PROVIDER}" CONFLUX_HTTP2_PROVIDER_UPPER)
if((CONFLUX_WANT_HTTP_SERVER OR CONFLUX_WANT_HTTP_CLIENT)
        AND NOT CONFLUX_HTTP2_PROVIDER_UPPER STREQUAL "OFF")
    conflux_pkg_provider(NGHTTP2 FALSE libnghttp2)
endif()
string(TOUPPER "${CONFLUX_HTTP3_PROVIDER}" CONFLUX_HTTP3_PROVIDER_UPPER)
if(CONFLUX_WANT_HTTP_SERVER
        AND CONFLUX_ENABLE_EXPERIMENTAL
        AND NOT CONFLUX_HTTP3_PROVIDER_UPPER STREQUAL "OFF")
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
string(TOUPPER "${CONFLUX_POSTGRES_PROVIDER}" CONFLUX_POSTGRES_PROVIDER_UPPER)
if(CONFLUX_WANT_DB_POSTGRES AND NOT CONFLUX_POSTGRES_PROVIDER_UPPER STREQUAL "OFF")
    conflux_pkg_provider(LIBPQ FALSE libpq)
    if(LIBPQ_FOUND)
        include(CheckIncludeFileCXX)
        set(_conflux_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
        get_target_property(_conflux_libpq_include_dirs PkgConfig::LIBPQ INTERFACE_INCLUDE_DIRECTORIES)
        if(_conflux_libpq_include_dirs)
            set(CMAKE_REQUIRED_INCLUDES "${_conflux_libpq_include_dirs}")
        endif()
        check_include_file_cxx("libpq-fe.h" CONFLUX_LIBPQ_HAS_HEADER)
        set(CMAKE_REQUIRED_INCLUDES "${_conflux_saved_required_includes}")
        if(NOT CONFLUX_LIBPQ_HAS_HEADER)
            set(LIBPQ_FOUND FALSE)
            message(STATUS "conflux: libpq pkg-config entry found, but libpq-fe.h is not usable")
        endif()
    endif()
endif()
