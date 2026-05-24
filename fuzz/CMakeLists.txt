# libFuzzer drivers. Each target links only the component it imports so the
# smoke preset can exercise parser helpers without pulling the io_uring server
# runtime. Requires the `fuzz-clang-stdcxx` preset.

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "fuzz targets require clang (libFuzzer)")
endif()

function(conflux_add_fuzz name)
    add_executable(${name} ${name}.cxx)
    target_link_libraries(${name} PRIVATE ${ARGN} conflux_options)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

set(CONFLUX_FUZZ_TARGETS)
set(CONFLUX_FUZZ_CORPUS_TARGETS)
set(CONFLUX_FUZZ_JSON_TARGETS)

if(TARGET conflux_http_parse_helpers)
    conflux_add_fuzz(fuzz_decode_chunked conflux_http_parse_helpers)
    conflux_add_fuzz(fuzz_urlencoded conflux_http_parse_helpers)
    list(APPEND CONFLUX_FUZZ_TARGETS fuzz_decode_chunked fuzz_urlencoded)
    list(APPEND CONFLUX_FUZZ_CORPUS_TARGETS fuzz_decode_chunked fuzz_urlencoded)
endif()

if(TARGET conflux_http1)
    conflux_add_fuzz(fuzz_http1_parser conflux_http1)
    list(APPEND CONFLUX_FUZZ_TARGETS fuzz_http1_parser)
    list(APPEND CONFLUX_FUZZ_CORPUS_TARGETS fuzz_http1_parser)
endif()

if(TARGET conflux_http_server_helpers)
    conflux_add_fuzz(fuzz_cookie_header conflux_http_server_helpers)
    conflux_add_fuzz(fuzz_multipart conflux_http_server_helpers)
    list(APPEND CONFLUX_FUZZ_TARGETS fuzz_cookie_header fuzz_multipart)
    list(APPEND CONFLUX_FUZZ_CORPUS_TARGETS fuzz_cookie_header fuzz_multipart)
endif()

if(TARGET conflux_http_static_core AND TARGET conflux_file_io_sync AND TARGET conflux_utils)
    conflux_add_fuzz(fuzz_static_path conflux_http_static_core conflux_file_io_sync conflux_utils)
    list(APPEND CONFLUX_FUZZ_TARGETS fuzz_static_path)
    list(APPEND CONFLUX_FUZZ_CORPUS_TARGETS fuzz_static_path)
endif()

if(TARGET conflux_http_auth)
    conflux_add_fuzz(fuzz_cookie_signing conflux_http_auth)
    list(APPEND CONFLUX_FUZZ_TARGETS fuzz_cookie_signing)
    list(APPEND CONFLUX_FUZZ_CORPUS_TARGETS fuzz_cookie_signing)
endif()

if(TARGET conflux_http_policy AND TARGET conflux_http_auth)
    conflux_add_fuzz(fuzz_security_policy conflux_http_policy conflux_http_auth)
    list(APPEND CONFLUX_FUZZ_TARGETS fuzz_security_policy)
    list(APPEND CONFLUX_FUZZ_CORPUS_TARGETS fuzz_security_policy)
endif()

if(TARGET conflux_http_realtime)
    conflux_add_fuzz(fuzz_utf8 conflux_http_realtime)
    conflux_add_fuzz(fuzz_ws_accept conflux_http_realtime)
    conflux_add_fuzz(fuzz_ws_frame conflux_http_realtime)
    list(APPEND CONFLUX_FUZZ_TARGETS fuzz_utf8 fuzz_ws_accept fuzz_ws_frame)
    list(APPEND CONFLUX_FUZZ_CORPUS_TARGETS fuzz_utf8 fuzz_ws_accept fuzz_ws_frame)
endif()

if(TARGET conflux_json)
    set(CONFLUX_FUZZ_JSON_TARGETS
        fuzz_json_parse
        fuzz_json5
        fuzz_json_reader
        fuzz_json_sax
        fuzz_ndjson
    )
    foreach(fz IN LISTS CONFLUX_FUZZ_JSON_TARGETS)
        conflux_add_fuzz(${fz} conflux_json)
    endforeach()
    list(APPEND CONFLUX_FUZZ_TARGETS ${CONFLUX_FUZZ_JSON_TARGETS})
endif()

if(NOT CONFLUX_FUZZ_TARGETS)
    message(FATAL_ERROR "CONFLUX_BUILD_FUZZ=ON but no fuzzable component targets were built")
endif()

add_custom_target(conflux_fuzz DEPENDS ${CONFLUX_FUZZ_TARGETS})

if(CONFLUX_BUILD_FUZZ_SMOKE_TESTS)
    foreach(fz IN LISTS CONFLUX_FUZZ_CORPUS_TARGETS)
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
