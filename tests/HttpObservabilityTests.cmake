if(TARGET conflux_http_observability)
    add_executable(conflux_http_metrics_e2e)
    target_sources(conflux_http_metrics_e2e
        PRIVATE
            http_metrics_e2e.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            support.cxx
    )
    target_include_directories(conflux_http_metrics_e2e PRIVATE "${PROJECT_SOURCE_DIR}/src/net")
    target_link_libraries(conflux_http_metrics_e2e
        PRIVATE
            conflux
            conflux_options
            Catch2::Catch2WithMain
    )

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_request_id_options
        SOURCE http_middleware_compile_fail_global_request_id_options.cxx
        TEST http-middleware/compile-fail-global-request-id-options
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "RequestIdOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_request_id_middleware
        SOURCE http_middleware_compile_fail_global_request_id_middleware.cxx
        TEST http-middleware/compile-fail-global-request-id-middleware
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "request_id_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_structured_log_options
        SOURCE http_middleware_compile_fail_global_structured_log_options.cxx
        TEST http-middleware/compile-fail-global-structured-log-options
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "StructuredLogOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_structured_log_middleware
        SOURCE http_middleware_compile_fail_global_structured_log_middleware.cxx
        TEST http-middleware/compile-fail-global-structured-log-middleware
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "structured_log_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_trace_context
        SOURCE http_middleware_compile_fail_global_trace_context.cxx
        TEST http-middleware/compile-fail-global-trace-context
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "TraceContext")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_tracing_context
        SOURCE http_middleware_compile_fail_global_tracing_context.cxx
        TEST http-middleware/compile-fail-global-tracing-context
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "TracingContext")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_tracing_options
        SOURCE http_middleware_compile_fail_global_tracing_options.cxx
        TEST http-middleware/compile-fail-global-tracing-options
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "TracingOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_tracing_middleware
        SOURCE http_middleware_compile_fail_global_tracing_middleware.cxx
        TEST http-middleware/compile-fail-global-tracing-middleware
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "tracing_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_counter
        SOURCE http_metrics_compile_fail_global_counter.cxx
        TEST http-metrics/compile-fail-global-counter
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "Counter")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_gauge
        SOURCE http_metrics_compile_fail_global_gauge.cxx
        TEST http-metrics/compile-fail-global-gauge
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "Gauge")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_histogram
        SOURCE http_metrics_compile_fail_global_histogram.cxx
        TEST http-metrics/compile-fail-global-histogram
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "Histogram")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_prometheus_label
        SOURCE http_metrics_compile_fail_global_prometheus_label.cxx
        TEST http-metrics/compile-fail-global-prometheus-label
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "PrometheusLabel")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_metrics_registry
        SOURCE http_metrics_compile_fail_global_metrics_registry.cxx
        TEST http-metrics/compile-fail-global-metrics-registry
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "MetricsRegistry")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_metrics_middleware
        SOURCE http_metrics_compile_fail_global_metrics_middleware.cxx
        TEST http-metrics/compile-fail-global-metrics-middleware
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "metrics_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_metrics_handler
        SOURCE http_metrics_compile_fail_global_metrics_handler.cxx
        TEST http-metrics/compile-fail-global-metrics-handler
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "metrics_handler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_metrics_handler_protected
        SOURCE http_metrics_compile_fail_global_metrics_handler_protected.cxx
        TEST http-metrics/compile-fail-global-metrics-handler-protected
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "metrics_handler_protected")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_pressure_formatter
        SOURCE http_metrics_compile_fail_global_pressure_formatter.cxx
        TEST http-metrics/compile-fail-global-pressure-formatter
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "format_pressure_metrics_prometheus")
endif()
