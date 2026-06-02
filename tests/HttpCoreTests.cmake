if(TARGET conflux_http_core)
    add_executable(http_request_assert_probe http_request_assert_probe.cxx)
    target_link_libraries(http_request_assert_probe
        PRIVATE conflux_http_core conflux_options)

    add_executable(conflux_http_core_tests)
    target_sources(conflux_http_core_tests
        PRIVATE
            http_core_test.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            assert_probe_support.cxx)
    target_link_libraries(conflux_http_core_tests
        PRIVATE conflux_http_core conflux_options Catch2::Catch2WithMain)
    target_compile_definitions(conflux_http_core_tests PRIVATE
        "ASSERT_PROBE_BIN=\"$<TARGET_FILE:http_request_assert_probe>\"")
    add_dependencies(conflux_http_core_tests http_request_assert_probe)

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_request
        SOURCE http_core_compile_fail_global_request.cxx
        TEST http-core/compile-fail-global-request
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "Request")

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_request_view
        SOURCE http_core_compile_fail_global_request_view.cxx
        TEST http-core/compile-fail-global-request-view
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "RequestView")

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_field_eq
        SOURCE http_core_compile_fail_global_field_eq.cxx
        TEST http-core/compile-fail-global-field-eq
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "FieldEq")

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_field_hash
        SOURCE http_core_compile_fail_global_field_hash.cxx
        TEST http-core/compile-fail-global-field-hash
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "FieldHash")

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_http_fields
        SOURCE http_core_compile_fail_global_http_fields.cxx
        TEST http-core/compile-fail-global-http-fields
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "HttpFields")

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_http_fields_view
        SOURCE http_core_compile_fail_global_http_fields_view.cxx
        TEST http-core/compile-fail-global-http-fields-view
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "HttpFieldsView")
endif()
