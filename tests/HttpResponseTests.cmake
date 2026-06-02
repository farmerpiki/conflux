if(TARGET conflux_http_response)
    add_executable(conflux_http_response_tests http_response_test.cxx)
    target_link_libraries(conflux_http_response_tests
        PRIVATE conflux_http_response conflux_options Catch2::Catch2WithMain)

    conflux_add_compile_fail_test(
        TARGET conflux_http_response_compile_fail_global_deferred_response
        SOURCE http_response_compile_fail_global_deferred_response.cxx
        TEST http-response/compile-fail-global-deferred-response
        LINK conflux_http_response conflux_options
        LABELS http compile-fail
        EXPECT "DeferredResponse")

    conflux_add_compile_fail_test(
        TARGET conflux_http_response_compile_fail_global_response
        SOURCE http_response_compile_fail_global_response.cxx
        TEST http-response/compile-fail-global-response
        LINK conflux_http_response conflux_options
        LABELS http compile-fail
        EXPECT "Response")
endif()
