# libFuzzer drivers. Each target links the instrumented `conflux` library
# plus the fuzzer runtime. Requires the `fuzz-clang-stdcxx` preset.

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "fuzz targets require clang (libFuzzer)")
endif()

set(CONFLUX_FUZZ_HTTP_TARGETS
    fuzz_decode_chunked
    fuzz_utf8
    fuzz_ws_accept
    fuzz_urlencoded
    fuzz_http1_parser
    fuzz_ws_frame
)
set(CONFLUX_FUZZ_JSON_TARGETS
    fuzz_json_parse
    fuzz_json5
    fuzz_json_reader
    fuzz_json_sax
    fuzz_ndjson
)
set(CONFLUX_FUZZ_TARGETS
    ${CONFLUX_FUZZ_HTTP_TARGETS}
    ${CONFLUX_FUZZ_JSON_TARGETS}
)

function(conflux_add_fuzz name)
    add_executable(${name} ${name}.cxx)
    target_link_libraries(${name} PRIVATE conflux conflux_options)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

foreach(fz IN LISTS CONFLUX_FUZZ_TARGETS)
    conflux_add_fuzz(${fz})
endforeach()

add_custom_target(conflux_fuzz DEPENDS ${CONFLUX_FUZZ_TARGETS})

if(CONFLUX_BUILD_FUZZ_SMOKE_TESTS)
    foreach(fz IN LISTS CONFLUX_FUZZ_HTTP_TARGETS)
        add_test(
            NAME ${fz}_smoke
            COMMAND ${fz} -runs=100000 -max_total_time=5
                          ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${fz}
        )
        set_tests_properties(${fz}_smoke PROPERTIES
            LABELS "fuzz-smoke"
            TIMEOUT 20)
    endforeach()

    foreach(fz IN LISTS CONFLUX_FUZZ_JSON_TARGETS)
        add_test(
            NAME ${fz}_smoke
            COMMAND ${fz} -runs=10000 -max_total_time=5
                          ${CMAKE_CURRENT_SOURCE_DIR}/corpus/fuzz_json_parse
        )
        set_tests_properties(${fz}_smoke PROPERTIES
            LABELS "fuzz-smoke"
            TIMEOUT 20)
    endforeach()
endif()
