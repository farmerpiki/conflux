if(TARGET conflux_http_parse_helpers)
    conflux_add_compile_fail_test(
        TARGET conflux_http_parse_helpers_compile_fail_global_chunk_state
        SOURCE http_parse_helpers_compile_fail_global_chunk_state.cxx
        TEST http-parse-helpers/compile-fail-global-chunk-state
        LINK conflux_http_parse_helpers conflux_options
        LABELS http compile-fail
        EXPECT "ChunkedDecodeState")

    conflux_add_compile_fail_test(
        TARGET conflux_http_parse_helpers_compile_fail_global_parse_urlencoded
        SOURCE http_parse_helpers_compile_fail_global_parse_urlencoded.cxx
        TEST http-parse-helpers/compile-fail-global-parse-urlencoded
        LINK conflux_http_parse_helpers conflux_options
        LABELS http compile-fail
        EXPECT "parse_urlencoded")

    conflux_add_compile_fail_test(
        TARGET conflux_http_parse_helpers_compile_fail_global_content_type
        SOURCE http_parse_helpers_compile_fail_global_content_type.cxx
        TEST http-parse-helpers/compile-fail-global-content-type
        LINK conflux_http_parse_helpers conflux_options
        LABELS http compile-fail
        EXPECT "content_type_media_type")
endif()
