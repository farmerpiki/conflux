if(TARGET conflux_http_router_match)
    conflux_add_compile_fail_test(
        TARGET conflux_http_router_match_compile_fail_global_segment
        SOURCE http_router_match_compile_fail_global_segment.cxx
        TEST http-router-match/compile-fail-global-segment
        LINK conflux_http_router_match conflux_options
        LABELS http compile-fail
        EXPECT "Segment")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_match_compile_fail_global_parse_pattern
        SOURCE http_router_match_compile_fail_global_parse_pattern.cxx
        TEST http-router-match/compile-fail-global-parse-pattern
        LINK conflux_http_router_match conflux_options
        LABELS http compile-fail
        EXPECT "parse_pattern")
endif()
