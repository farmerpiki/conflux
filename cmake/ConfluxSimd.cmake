# SIMD and ISA-specific fast-path detection.

include(CheckCXXSourceCompiles)

function(conflux_detect_aesni out_var)
    set(_conflux_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "-maes -mpclmul -mssse3 -msse4.1")
    set(_conflux_aesni_test_code "
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
    check_cxx_source_compiles("${_conflux_aesni_test_code}" ${out_var})
    set(CMAKE_REQUIRED_FLAGS "${_conflux_saved_required_flags}")
endfunction()

function(conflux_apply_aesni_source_options source)
    set_source_files_properties("${source}" PROPERTIES
        COMPILE_OPTIONS "-maes;-mpclmul;-mssse3;-msse4.1")
endfunction()

function(conflux_detect_stdsimd out_var)
    if(NOT CONFLUX_USE_STDSIMD MATCHES "^(AUTO|STD26|STDX|ON|OFF)$")
        message(FATAL_ERROR
            "conflux: CONFLUX_USE_STDSIMD must be AUTO, STD26, STDX, ON, or OFF "
            "(got '${CONFLUX_USE_STDSIMD}')")
    endif()

    set(_conflux_simd_backend OFF)
    if(CONFLUX_USE_STDSIMD STREQUAL "AUTO" OR CONFLUX_USE_STDSIMD STREQUAL "STD26")
        set(_conflux_stdsimd_std26_test_code "
#include <simd>
#include <cstddef>
#include <span>
using vec_t = std::simd::vec<signed char>;
using uvec_t = std::simd::vec<unsigned char>;
int main() {
    vec_t a{};
    vec_t b{};
    auto mask = (a == b) | (a < b);
    if (std::simd::any_of(mask))
        return static_cast<int>(std::simd::reduce_min_index(mask));
    unsigned char bytes[uvec_t::size] = {};
    auto u = std::simd::unchecked_load<uvec_t>(std::span<unsigned char>(bytes, uvec_t::size));
    u = std::simd::select(u == uvec_t{}, u | uvec_t(static_cast<unsigned char>(1)), u);
    std::simd::unchecked_store(u, std::span<unsigned char>(bytes, uvec_t::size));
    return static_cast<int>(vec_t::size);
}")
        set(_conflux_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
        set(_conflux_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
        set(CMAKE_CXX_STANDARD 26)
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
        check_cxx_source_compiles("${_conflux_stdsimd_std26_test_code}" CONFLUX_HAS_SIMD_STD26)
        set(CMAKE_CXX_STANDARD "${_conflux_saved_cxx_standard}")
        set(CMAKE_REQUIRED_FLAGS "${_conflux_saved_required_flags}")
        if(CONFLUX_HAS_SIMD_STD26)
            set(_conflux_simd_backend STD26)
        elseif(CONFLUX_USE_STDSIMD STREQUAL "STD26")
            message(FATAL_ERROR "conflux: CONFLUX_USE_STDSIMD=STD26 requires C++26 <simd> support")
        endif()
    endif()

    if((_conflux_simd_backend STREQUAL "OFF")
            AND (CONFLUX_USE_STDSIMD STREQUAL "AUTO"
                OR CONFLUX_USE_STDSIMD STREQUAL "STDX"
                OR CONFLUX_USE_STDSIMD STREQUAL "ON"))
        set(_conflux_stdsimd_stdx_test_code "
#include <experimental/simd>
#include <cstddef>
namespace stdx = std::experimental::parallelism_v2;
using vec_t = stdx::native_simd<signed char>;
using uvec_t = stdx::native_simd<unsigned char>;
int main() {
    vec_t a{};
    vec_t b{};
    auto mask = (a == b) | (a < b);
    if (stdx::any_of(mask))
        return static_cast<int>(stdx::find_first_set(mask));
    unsigned char bytes[uvec_t::size()] = {};
    uvec_t u(bytes, stdx::element_aligned);
    stdx::where(u == uvec_t{}, u) = u | uvec_t(static_cast<unsigned char>(1));
    u.copy_to(bytes, stdx::element_aligned);
    return static_cast<int>(vec_t::size());
}")
        set(_conflux_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
        check_cxx_source_compiles("${_conflux_stdsimd_stdx_test_code}" CONFLUX_HAS_SIMD_STDX)
        set(CMAKE_REQUIRED_FLAGS "${_conflux_saved_required_flags}")
        if(CONFLUX_HAS_SIMD_STDX)
            set(_conflux_simd_backend STDX)
        elseif(CONFLUX_USE_STDSIMD STREQUAL "STDX" OR CONFLUX_USE_STDSIMD STREQUAL "ON")
            message(FATAL_ERROR "conflux: CONFLUX_USE_STDSIMD=${CONFLUX_USE_STDSIMD} requires <experimental/simd> support")
        endif()
    endif()

    set(${out_var} "${_conflux_simd_backend}" PARENT_SCOPE)
endfunction()

function(conflux_add_stdsimd_targets backend)
    set(_conflux_json_stdsimd_ifunc OFF)
    if(_conflux_simd_selection STREQUAL "RUNTIME"
            AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
            AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
            AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
        set(_conflux_json_stdsimd_ifunc ON)
    endif()
    set(CONFLUX_JSON_STDSIMD_IFUNC "${_conflux_json_stdsimd_ifunc}" PARENT_SCOPE)

    if(backend STREQUAL "STD26")
        add_library(conflux_simd_std26 OBJECT ${CONFLUX_SRC_ROOT}/simd_std26.cxx)
        target_compile_features(conflux_simd_std26 PRIVATE cxx_std_26)
        set(_conflux_simd_std26_options "-mavx2")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
                AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16
                AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
            # GCC 16 currently fails the std::simd C++26 object under LTO.
            # Keep the fallback scoped to the affected object target/source.
            list(APPEND _conflux_simd_std26_options "-fno-lto")
            set_target_properties(conflux_simd_std26 PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
        endif()
        set_source_files_properties(${CONFLUX_SRC_ROOT}/simd_std26.cxx PROPERTIES
            COMPILE_OPTIONS "${_conflux_simd_std26_options}")

        add_library(conflux_json_simd_std26 OBJECT ${CONFLUX_SRC_ROOT}/json_simd_std26.cxx)
        target_compile_features(conflux_json_simd_std26 PRIVATE cxx_std_26)
        set(_conflux_json_simd_std26_options "-mavx2")
        if(_conflux_json_stdsimd_ifunc)
            target_compile_definitions(conflux_json_simd_std26 PRIVATE
                CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD=conflux_json_scan_str_until_special_stdsimd_avx2
                CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD=conflux_json_scan_dump_safe_run_stdsimd_avx2)
            add_library(conflux_json_simd_std26_sse2 OBJECT ${CONFLUX_SRC_ROOT}/json_simd_std26.cxx)
            target_compile_features(conflux_json_simd_std26_sse2 PRIVATE cxx_std_26)
            target_compile_definitions(conflux_json_simd_std26_sse2 PRIVATE
                CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD=conflux_json_scan_str_until_special_stdsimd_sse2
                CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD=conflux_json_scan_dump_safe_run_stdsimd_sse2)
            target_compile_options(conflux_json_simd_std26_sse2 PRIVATE "-msse2")
        endif()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
                AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16
                AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
            # GCC 16 currently fails the std::simd JSON scanner objects under
            # LTO; leave other SIMD backends and non-GNU lanes untouched.
            list(APPEND _conflux_json_simd_std26_options "-fno-lto")
            set_target_properties(conflux_json_simd_std26 PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
            if(_conflux_json_stdsimd_ifunc)
                target_compile_options(conflux_json_simd_std26_sse2 PRIVATE "-fno-lto")
                set_target_properties(conflux_json_simd_std26_sse2 PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
            endif()
        endif()
        target_compile_options(conflux_json_simd_std26 PRIVATE
            ${_conflux_json_simd_std26_options})
    elseif(backend STREQUAL "STDX")
        add_library(conflux_simd_stdx OBJECT ${CONFLUX_SRC_ROOT}/simd_stdx.cxx)
        target_compile_features(conflux_simd_stdx PRIVATE cxx_std_23)
        target_link_libraries(conflux_simd_stdx PRIVATE conflux_options)
        target_compile_options(conflux_simd_stdx PRIVATE "-mavx2")

        add_library(conflux_json_simd_stdx OBJECT ${CONFLUX_SRC_ROOT}/json_simd_stdx.cxx)
        target_compile_features(conflux_json_simd_stdx PRIVATE cxx_std_23)
        target_link_libraries(conflux_json_simd_stdx PRIVATE conflux_options)
        if(_conflux_json_stdsimd_ifunc)
            target_compile_definitions(conflux_json_simd_stdx PRIVATE
                CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD=conflux_json_scan_str_until_special_stdsimd_avx2
                CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD=conflux_json_scan_dump_safe_run_stdsimd_avx2)
            add_library(conflux_json_simd_stdx_sse2 OBJECT ${CONFLUX_SRC_ROOT}/json_simd_stdx.cxx)
            target_compile_features(conflux_json_simd_stdx_sse2 PRIVATE cxx_std_23)
            target_link_libraries(conflux_json_simd_stdx_sse2 PRIVATE conflux_options)
            target_compile_definitions(conflux_json_simd_stdx_sse2 PRIVATE
                CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD=conflux_json_scan_str_until_special_stdsimd_sse2
                CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD=conflux_json_scan_dump_safe_run_stdsimd_sse2)
            target_compile_options(conflux_json_simd_stdx_sse2 PRIVATE "-msse2")
        endif()
        target_compile_options(conflux_json_simd_stdx PRIVATE "-mavx2")
    endif()

    if(NOT backend STREQUAL "OFF")
        string(TOLOWER "${backend}" _conflux_simd_target_suffix)
        add_library(conflux_simd_runtime STATIC $<TARGET_OBJECTS:conflux_simd_${_conflux_simd_target_suffix}>)
        target_link_libraries(conflux_simd_runtime PRIVATE conflux_options)
        if(_conflux_json_stdsimd_ifunc)
            add_library(conflux_json_simd_ifunc OBJECT ${CONFLUX_SRC_ROOT}/json_simd_ifunc.cxx)
            target_link_libraries(conflux_json_simd_ifunc PRIVATE conflux_options)
        endif()
    endif()
endfunction()

function(conflux_attach_stdsimd_targets backend)
    if(backend STREQUAL "OFF")
        return()
    endif()

    if(TARGET conflux_crypto)
        target_compile_definitions(conflux_crypto PRIVATE CONFLUX_STDSIMD=1)
        target_link_libraries(conflux_crypto PRIVATE conflux_simd_runtime)
    endif()

    if(TARGET conflux_json)
        message(STATUS "conflux: ${backend} SIMD path enabled (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
        target_compile_definitions(conflux_json PRIVATE CONFLUX_JSON_USE_STDSIMD=1)
        string(TOLOWER "${backend}" _conflux_json_simd_target_suffix)
        target_sources(conflux_json PRIVATE $<TARGET_OBJECTS:conflux_json_simd_${_conflux_json_simd_target_suffix}>)
        if(CONFLUX_JSON_STDSIMD_IFUNC)
            message(STATUS "conflux: JSON runtime SIMD dispatch uses ELF IFUNC")
            target_compile_definitions(conflux_json PRIVATE CONFLUX_JSON_STDSIMD_IFUNC=1)
            target_sources(conflux_json PRIVATE
                $<TARGET_OBJECTS:conflux_json_simd_${_conflux_json_simd_target_suffix}_sse2>
                $<TARGET_OBJECTS:conflux_json_simd_ifunc>)
        endif()
    endif()
endfunction()
