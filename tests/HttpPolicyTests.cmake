if(TARGET conflux_http_policy)
    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_cache_control_options
        SOURCE http_policy_compile_fail_global_cache_control_options.cxx
        TEST http-policy/compile-fail-global-cache-control-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "CacheControlOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_forwarded_options
        SOURCE http_policy_compile_fail_global_forwarded_options.cxx
        TEST http-policy/compile-fail-global-forwarded-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "ForwardedOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_ip_filter_options
        SOURCE http_policy_compile_fail_global_ip_filter_options.cxx
        TEST http-policy/compile-fail-global-ip-filter-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "IpFilterOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_rate_limit_options
        SOURCE http_policy_compile_fail_global_rate_limit_options.cxx
        TEST http-policy/compile-fail-global-rate-limit-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "RateLimitOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_redirect_options
        SOURCE http_policy_compile_fail_global_redirect_options.cxx
        TEST http-policy/compile-fail-global-redirect-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "RedirectOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_response_cache_options
        SOURCE http_policy_compile_fail_global_response_cache_options.cxx
        TEST http-policy/compile-fail-global-response-cache-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "ResponseCacheOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_security_options
        SOURCE http_policy_compile_fail_global_security_options.cxx
        TEST http-policy/compile-fail-global-security-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "SecurityOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_trailing_slash_options
        SOURCE http_policy_compile_fail_global_trailing_slash_options.cxx
        TEST http-policy/compile-fail-global-trailing-slash-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "TrailingSlashOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_cors_options
        SOURCE http_middleware_compile_fail_global_cors_options.cxx
        TEST http-middleware/compile-fail-global-cors-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "CorsOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_cors_default_max_age
        SOURCE http_middleware_compile_fail_global_cors_default_max_age.cxx
        TEST http-middleware/compile-fail-global-cors-default-max-age
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "kCorsDefaultMaxAge")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_cors_middleware
        SOURCE http_middleware_compile_fail_global_cors_middleware.cxx
        TEST http-middleware/compile-fail-global-cors-middleware
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "cors_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_etag_options
        SOURCE http_middleware_compile_fail_global_etag_options.cxx
        TEST http-middleware/compile-fail-global-etag-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "ETagOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_etag_middleware
        SOURCE http_middleware_compile_fail_global_etag_middleware.cxx
        TEST http-middleware/compile-fail-global-etag-middleware
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "etag_middleware")
endif()
