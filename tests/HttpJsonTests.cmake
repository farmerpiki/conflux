if(TARGET conflux_http_response_json AND TARGET conflux_http_app_json AND TARGET conflux_http_native_json)
    add_executable(conflux_http_json_tests http_json_test.cxx)
    target_link_libraries(conflux_http_json_tests
        PRIVATE conflux_http_app_json conflux_http_native_json conflux_options Catch2::Catch2WithMain)

    conflux_add_compile_fail_test(
        TARGET conflux_http_json_compile_fail_provider_template_response_alias
        SOURCE http_json_compile_fail_provider_template_response_alias.cxx
        TEST http-json/compile-fail-provider-template-response-alias
        LINK conflux_http_native_json conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_json_unexpected_provider_template_response_alias_visible")
endif()
