if(TARGET conflux_http_server_helpers)
    add_executable(conflux_http_server_helpers_tests http_server_helpers_test.cxx)
    target_link_libraries(conflux_http_server_helpers_tests
        PRIVATE conflux_http_server_helpers conflux_http_realtime conflux_options Catch2::Catch2WithMain)

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_helpers_compile_fail_global_format_response
        SOURCE http_server_helpers_compile_fail_global_format_response.cxx
        TEST http-server-helpers/compile-fail-global-format-response
        LINK conflux_http_server_helpers conflux_options
        LABELS http compile-fail
        EXPECT "format_response")

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_helpers_compile_fail_global_expect_state
        SOURCE http_server_helpers_compile_fail_global_expect_state.cxx
        TEST http-server-helpers/compile-fail-global-expect-state
        LINK conflux_http_server_helpers conflux_options
        LABELS http compile-fail
        EXPECT "ExpectState")

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_helpers_compile_fail_global_parse_cookies
        SOURCE http_server_helpers_compile_fail_global_parse_cookies.cxx
        TEST http-server-helpers/compile-fail-global-parse-cookies
        LINK conflux_http_server_helpers conflux_options
        LABELS http compile-fail
        EXPECT "parse_cookies")
endif()
