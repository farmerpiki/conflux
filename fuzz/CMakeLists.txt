# libFuzzer drivers. Each target links the instrumented `conflux` library
# plus the fuzzer runtime. Requires the `fuzz-clang-libcxx` preset.

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "fuzz targets require clang (libFuzzer)")
endif()

function(conflux_add_fuzz name)
    add_executable(${name} ${name}.cxx)
    target_link_libraries(${name} PRIVATE conflux conflux_options)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

conflux_add_fuzz(fuzz_decode_chunked)
conflux_add_fuzz(fuzz_utf8)
conflux_add_fuzz(fuzz_ws_accept)
conflux_add_fuzz(fuzz_urlencoded)
conflux_add_fuzz(fuzz_http1_parser)
conflux_add_fuzz(fuzz_ws_frame)
conflux_add_fuzz(fuzz_json_parse)
conflux_add_fuzz(fuzz_json5)
conflux_add_fuzz(fuzz_json_reader)
conflux_add_fuzz(fuzz_json_sax)
conflux_add_fuzz(fuzz_ndjson)

if(CONFLUX_BUILD_TESTS)
    enable_testing()
    foreach(fz IN ITEMS fuzz_decode_chunked fuzz_utf8 fuzz_ws_accept fuzz_urlencoded fuzz_http1_parser fuzz_ws_frame)
        add_test(
            NAME ${fz}_smoke
            COMMAND ${fz} -runs=100000 -max_total_time=5
                          ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${fz}
        )
        set_tests_properties(${fz}_smoke PROPERTIES LABELS "fuzz-smoke")
    endforeach()

    foreach(fz IN ITEMS fuzz_json_parse fuzz_json5 fuzz_json_reader fuzz_json_sax fuzz_ndjson)
        add_test(
            NAME ${fz}_smoke
            COMMAND ${fz} -runs=10000 -max_total_time=5
                          ${CMAKE_CURRENT_SOURCE_DIR}/corpus/fuzz_json_parse
        )
        set_tests_properties(${fz}_smoke PROPERTIES LABELS "fuzz-smoke")
    endforeach()
endif()
