if(TARGET conflux_net_http)
    add_executable(conflux_http_facade_tests http_facade_test.cxx)
    target_link_libraries(conflux_http_facade_tests
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)
    add_executable(conflux_http_facade_extractors_tests http_facade_extractors_test.cxx)
    target_link_libraries(conflux_http_facade_extractors_tests
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)
    add_executable(conflux_http_facade_observability_tests http_facade_observability_test.cxx)
    target_link_libraries(conflux_http_facade_observability_tests
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)
    add_executable(conflux_http_facade_openapi_tests http_facade_openapi_test.cxx)
    target_link_libraries(conflux_http_facade_openapi_tests
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)
    add_executable(conflux_http_facade_routes_tests http_facade_routes_test.cxx)
    target_link_libraries(conflux_http_facade_routes_tests
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)
    add_executable(conflux_http_facade_response_tests http_facade_response_test.cxx)
    target_link_libraries(conflux_http_facade_response_tests
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)
    add_executable(conflux_http_facade_validation_tests http_facade_validation_test.cxx)
    target_link_libraries(conflux_http_facade_validation_tests
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
        # GCC 16 ICEs during LTO for these facade test TUs; keep the fallback to the sources that trigger it.
        set_source_files_properties(
            http_facade_test.cxx
            http_facade_extractors_test.cxx
            http_facade_observability_test.cxx
            http_facade_openapi_test.cxx
            http_facade_response_test.cxx
            http_facade_routes_test.cxx
            http_facade_validation_test.cxx
            PROPERTIES COMPILE_OPTIONS "-fno-lto")
    endif()

    add_executable(conflux_http_facade_import_smoke http_facade_import_smoke.cxx)
    target_link_libraries(conflux_http_facade_import_smoke
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)

    add_library(conflux_http_facade_api_snapshot OBJECT http_facade_api_snapshot.cxx)
    target_link_libraries(conflux_http_facade_api_snapshot
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_extended_api_snapshot OBJECT http_facade_extended_api_snapshot.cxx)
    target_link_libraries(conflux_http_facade_extended_api_snapshot
        PRIVATE conflux_net_http conflux_options)

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_raw_string
        SOURCE http_facade_compile_fail_raw_string.cxx
        TEST http-facade/compile-fail-raw-string
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HTTP app handlers must not return raw strings" "use http::text(...)" "http::html(...)")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_raw_string_extractor
        SOURCE http_facade_compile_fail_raw_string_extractor.cxx
        TEST http-facade/compile-fail-raw-string-extractor
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HTTP app handlers must not return raw strings" "use http::text(...)" "http::html(...)")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_json_codec
        SOURCE http_facade_compile_fail_json_codec.cxx
        TEST http-facade/compile-fail-json-codec
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "http::Json<T> responses require T to be serializable"
               "add conflux::json::JsonCodec<T>, conflux::json::JsonMembers<T>, or reflection JSON support")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_legacy_bearer
        SOURCE http_facade_compile_fail_legacy_bearer.cxx
        TEST http-facade/compile-fail-legacy-bearer
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RequiredBearer")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_cookie_builder
        SOURCE http_facade_compile_fail_global_cookie_builder.cxx
        TEST http-facade/compile-fail-global-cookie-builder
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "CookieBuilder")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_streamed_file
        SOURCE http_facade_compile_fail_global_streamed_file.cxx
        TEST http-facade/compile-fail-global-streamed-file
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "StreamedFile")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_response
        SOURCE http_facade_compile_fail_global_response.cxx
        TEST http-facade/compile-fail-global-response
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Response")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_uploaded_file
        SOURCE http_facade_compile_fail_global_uploaded_file.cxx
        TEST http-facade/compile-fail-global-uploaded-file
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "UploadedFile")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_cloneable_function
        SOURCE http_facade_compile_fail_global_cloneable_function.cxx
        TEST http-facade/compile-fail-global-cloneable-function
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "CloneableFunction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_method
        SOURCE http_facade_compile_fail_global_http_method.cxx
        TEST http-facade/compile-fail-global-http-method
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpMethod")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_request_context
        SOURCE http_facade_compile_fail_global_request_context.cxx
        TEST http-facade/compile-fail-global-request-context
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RequestContext")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_run_status
        SOURCE http_facade_compile_fail_global_run_status.cxx
        TEST http-facade/compile-fail-global-run-status
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RunStatus")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_config
        SOURCE http_facade_compile_fail_global_config.cxx
        TEST http-facade/compile-fail-global-config
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Config")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_parser_limits
        SOURCE http_facade_compile_fail_global_parser_limits.cxx
        TEST http-facade/compile-fail-global-parser-limits
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ParserLimits")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_config_issue
        SOURCE http_facade_compile_fail_global_config_issue.cxx
        TEST http-facade/compile-fail-global-config-issue
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ConfigIssue")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_drain_options
        SOURCE http_facade_compile_fail_global_drain_options.cxx
        TEST http-facade/compile-fail-global-drain-options
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "DrainOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_drain_report
        SOURCE http_facade_compile_fail_global_drain_report.cxx
        TEST http-facade/compile-fail-global-drain-report
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "DrainReport")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_drain_stream_policy
        SOURCE http_facade_compile_fail_global_drain_stream_policy.cxx
        TEST http-facade/compile-fail-global-drain-stream-policy
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "DrainStreamPolicy")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_overflow_policy
        SOURCE http_facade_compile_fail_global_overflow_policy.cxx
        TEST http-facade/compile-fail-global-overflow-policy
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "OverflowPolicy")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_metrics
        SOURCE http_facade_compile_fail_global_send_zc_metrics.cxx
        TEST http-facade/compile-fail-global-send-zc-metrics
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcMetrics")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_pending_action
        SOURCE http_facade_compile_fail_global_send_zc_pending_action.cxx
        TEST http-facade/compile-fail-global-send-zc-pending-action
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcPendingAction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_cqe_action
        SOURCE http_facade_compile_fail_global_send_zc_cqe_action.cxx
        TEST http-facade/compile-fail-global-send-zc-cqe-action
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcCqeAction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_cqe_state
        SOURCE http_facade_compile_fail_global_send_zc_cqe_state.cxx
        TEST http-facade/compile-fail-global-send-zc-cqe-state
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcCqeState")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_cqe_input
        SOURCE http_facade_compile_fail_global_send_zc_cqe_input.cxx
        TEST http-facade/compile-fail-global-send-zc-cqe-input
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcCqeInput")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_cqe_outcome
        SOURCE http_facade_compile_fail_global_send_zc_cqe_outcome.cxx
        TEST http-facade/compile-fail-global-send-zc-cqe-outcome
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcCqeOutcome")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_field_source
        SOURCE http_facade_compile_fail_global_http_field_source.cxx
        TEST http-facade/compile-fail-global-http-field-source
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpFieldSource")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_field_error_kind
        SOURCE http_facade_compile_fail_global_http_field_error_kind.cxx
        TEST http-facade/compile-fail-global-http-field-error-kind
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpFieldErrorKind")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_field_error
        SOURCE http_facade_compile_fail_global_http_field_error.cxx
        TEST http-facade/compile-fail-global-http-field-error
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpFieldError")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_reject_reason
        SOURCE http_facade_compile_fail_global_http_reject_reason.cxx
        TEST http-facade/compile-fail-global-http-reject-reason
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpRejectReason")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_rejection_metrics
        SOURCE http_facade_compile_fail_global_http_rejection_metrics.cxx
        TEST http-facade/compile-fail-global-http-rejection-metrics
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpRejectionMetrics")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_pressure_metrics
        SOURCE http_facade_compile_fail_global_http_pressure_metrics.cxx
        TEST http-facade/compile-fail-global-http-pressure-metrics
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpPressureMetrics")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_server_metrics
        SOURCE http_facade_compile_fail_global_http_server_metrics.cxx
        TEST http-facade/compile-fail-global-http-server-metrics
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpServerMetrics")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_server_observability_hooks
        SOURCE http_facade_compile_fail_global_http_server_observability_hooks.cxx
        TEST http-facade/compile-fail-global-http-server-observability-hooks
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpServerObservabilityHooks")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_sse_channel
        SOURCE http_facade_compile_fail_global_sse_channel.cxx
        TEST http-facade/compile-fail-global-sse-channel
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SseChannel")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_sse_overflow_policy
        SOURCE http_facade_compile_fail_global_sse_overflow_policy.cxx
        TEST http-facade/compile-fail-global-sse-overflow-policy
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SseOverflowPolicy")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_ws_conn
        SOURCE http_facade_compile_fail_global_ws_conn.cxx
        TEST http-facade/compile-fail-global-ws-conn
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "WsConn")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_ws_upgrade
        SOURCE http_facade_compile_fail_global_ws_upgrade.cxx
        TEST http-facade/compile-fail-global-ws-upgrade
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "WsUpgrade")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_ws_detail
        SOURCE http_facade_compile_fail_global_ws_detail.cxx
        TEST http-facade/compile-fail-global-ws-detail
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ws_detail")

    conflux_add_compile_fail_test(
        TARGET conflux_json_compile_fail_global_customization
        SOURCE json_compile_fail_global_customization.cxx
        TEST json/compile-fail-global-customization
        LINK conflux_json conflux_options
        LABELS json compile-fail
        EXPECT "JsonMembers")

    conflux_add_compile_fail_test(
        TARGET conflux_json_compile_fail_global_document
        SOURCE json_compile_fail_global_document.cxx
        TEST json/compile-fail-global-document
        LINK conflux_json conflux_options
        LABELS json compile-fail
        EXPECT "Document")

    conflux_add_compile_fail_test(
        TARGET conflux_json_compile_fail_global_default_handler
        SOURCE json_compile_fail_global_default_handler.cxx
        TEST json/compile-fail-global-default-handler
        LINK conflux_json conflux_options
        LABELS json compile-fail
        EXPECT "JsonDefaultHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_json_compile_fail_global_make_object
        SOURCE json_compile_fail_global_make_object.cxx
        TEST json/compile-fail-global-make-object
        LINK conflux_json conflux_options
        LABELS json compile-fail
        EXPECT "make_object")

    conflux_add_compile_fail_test(
        TARGET conflux_json_compile_fail_global_ndjson_range
        SOURCE json_compile_fail_global_ndjson_range.cxx
        TEST json/compile-fail-global-ndjson-range
        LINK conflux_json conflux_options
        LABELS json compile-fail
        EXPECT "NdjsonRange")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_router_alias
        SOURCE http_facade_compile_fail_router_alias.cxx
        TEST http-facade/compile-fail-router-alias
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_facade_unexpected_router_alias_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_route_policy_internal
        SOURCE http_facade_compile_fail_route_policy_internal.cxx
        TEST http-facade/compile-fail-route-policy-internal
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "AppRouteRateLimit")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_middleware_concept_alias
        SOURCE http_facade_compile_fail_middleware_concept_alias.cxx
        TEST http-facade/compile-fail-middleware-concept-alias
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_use_async_member
        SOURCE http_facade_compile_fail_use_async_member.cxx
        TEST http-facade/compile-fail-use-async-member
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "use_async" "conflux::http::App")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_use_async_helper
        SOURCE http_facade_compile_fail_use_async_helper.cxx
        TEST http-facade/compile-fail-use-async-helper
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "use_async")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_auth_policy_member
        SOURCE http_facade_compile_fail_auth_policy_member.cxx
        TEST http-facade/compile-fail-auth-policy-member
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "auth_policy")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_openapi_handler_member
        SOURCE http_facade_compile_fail_openapi_handler_member.cxx
        TEST http-facade/compile-fail-openapi-handler-member
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_facade_unexpected_openapi_handler_member_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_router_member
        SOURCE http_facade_compile_fail_router_member.cxx
        TEST http-facade/compile-fail-router-member
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_facade_unexpected_router_member_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_route_infos_member
        SOURCE http_facade_compile_fail_route_infos_member.cxx
        TEST http-facade/compile-fail-route-infos-member
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_facade_unexpected_route_infos_member_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_offload_free
        SOURCE http_facade_compile_fail_offload_free.cxx
        TEST http-facade/compile-fail-offload-free
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "offload")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_openapi_handler_free
        SOURCE http_facade_compile_fail_openapi_handler_free.cxx
        TEST http-facade/compile-fail-openapi-handler-free
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "openapi_handler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_openapi_compile_fail_global_openapi_handler
        SOURCE http_openapi_compile_fail_global_openapi_handler.cxx
        TEST http-openapi/compile-fail-global-openapi-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "openapi_handler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_file_free
        SOURCE http_facade_compile_fail_file_free.cxx
        TEST http-facade/compile-fail-file-free
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "file")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_extended_compile_fail_global_workpool
        SOURCE http_facade_extended_compile_fail_global_workpool.cxx
        TEST http-facade-extended/compile-fail-global-workpool
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_extended_unexpected_workpool_global_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_extended_compile_fail_global_router
        SOURCE http_facade_extended_compile_fail_global_router.cxx
        TEST http-facade-extended/compile-fail-global-router
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Router")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_extended_compile_fail_global_route_info
        SOURCE http_facade_extended_compile_fail_global_route_info.cxx
        TEST http-facade-extended/compile-fail-global-route-info
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RouteInfo")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_route_info
        SOURCE http_router_compile_fail_global_route_info.cxx
        TEST http-router/compile-fail-global-route-info
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RouteInfo")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_router
        SOURCE http_router_compile_fail_global_router.cxx
        TEST http-router/compile-fail-global-router
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Router")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_handler_result
        SOURCE http_router_compile_fail_global_handler_result.cxx
        TEST http-router/compile-fail-global-handler-result
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HandlerResult")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_view_handler
        SOURCE http_router_compile_fail_global_view_handler.cxx
        TEST http-router/compile-fail-global-view-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ViewHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_request_handler
        SOURCE http_router_compile_fail_global_request_handler.cxx
        TEST http-router/compile-fail-global-request-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RequestHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_route_handler
        SOURCE http_router_compile_fail_global_route_handler.cxx
        TEST http-router/compile-fail-global-route-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RouteHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_context_handler_function
        SOURCE http_router_compile_fail_global_context_handler_function.cxx
        TEST http-router/compile-fail-global-context-handler-function
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ContextHandlerFunction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_context_middleware_function
        SOURCE http_router_compile_fail_global_context_middleware_function.cxx
        TEST http-router/compile-fail-global-context-middleware-function
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ContextMiddlewareFunction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_async_middleware
        SOURCE http_router_compile_fail_global_async_middleware.cxx
        TEST http-router/compile-fail-global-async-middleware
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "AsyncMiddleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_view_middleware
        SOURCE http_router_compile_fail_global_view_middleware.cxx
        TEST http-router/compile-fail-global-view-middleware
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ViewMiddleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_request_middleware
        SOURCE http_router_compile_fail_global_request_middleware.cxx
        TEST http-router/compile-fail-global-request-middleware
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RequestMiddleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_middleware
        SOURCE http_router_compile_fail_global_middleware.cxx
        TEST http-router/compile-fail-global-middleware
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_next_handler
        SOURCE http_router_compile_fail_global_next_handler.cxx
        TEST http-router/compile-fail-global-next-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "NextHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_middleware_function
        SOURCE http_router_compile_fail_global_middleware_function.cxx
        TEST http-router/compile-fail-global-middleware-function
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "MiddlewareFunction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_context_next_handler
        SOURCE http_router_compile_fail_global_context_next_handler.cxx
        TEST http-router/compile-fail-global-context-next-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ContextNextHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_static_compile_fail_global_static_route_handler
        SOURCE http_router_static_compile_fail_global_static_route_handler.cxx
        TEST http-router-static/compile-fail-global-static-route-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "StaticRouteHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_static_compile_fail_global_static_route_registration
        SOURCE http_router_static_compile_fail_global_static_route_registration.cxx
        TEST http-router-static/compile-fail-global-static-route-registration
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "StaticRouteRegistration")

    conflux_add_compile_fail_test(
        TARGET conflux_http_static_compile_fail_global_static_options
        SOURCE http_static_compile_fail_global_static_options.cxx
        TEST http-static/compile-fail-global-static-options
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "StaticOptions")
endif()
