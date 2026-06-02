if(TARGET conflux_http_auth)
    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_basic_auth_options
        SOURCE http_auth_compile_fail_global_basic_auth_options.cxx
        TEST http-auth/compile-fail-global-basic-auth-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "BasicAuthOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_basic_credentials
        SOURCE http_auth_compile_fail_global_basic_credentials.cxx
        TEST http-auth/compile-fail-global-basic-credentials
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "BasicCredentials")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_parse_basic_credentials
        SOURCE http_auth_compile_fail_global_parse_basic_credentials.cxx
        TEST http-auth/compile-fail-global-parse-basic-credentials
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "parse_basic_credentials")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_failure_limiter
        SOURCE http_auth_compile_fail_global_auth_failure_limiter.cxx
        TEST http-auth/compile-fail-global-auth-failure-limiter
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "AuthFailureLimiter")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_options
        SOURCE http_auth_compile_fail_global_auth_throttle_options.cxx
        TEST http-auth/compile-fail-global-auth-throttle-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "AuthThrottleOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_outcome
        SOURCE http_auth_compile_fail_global_auth_throttle_outcome.cxx
        TEST http-auth/compile-fail-global-auth-throttle-outcome
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "AuthThrottleOutcome")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_metrics
        SOURCE http_auth_compile_fail_global_auth_throttle_metrics.cxx
        TEST http-auth/compile-fail-global-auth-throttle-metrics
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "AuthThrottleMetrics")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_middleware_options
        SOURCE http_auth_compile_fail_global_auth_throttle_middleware_options.cxx
        TEST http-auth/compile-fail-global-auth-throttle-middleware-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "AuthThrottleMiddlewareOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_key
        SOURCE http_auth_compile_fail_global_auth_throttle_key.cxx
        TEST http-auth/compile-fail-global-auth-throttle-key
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_key")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_remote_key
        SOURCE http_auth_compile_fail_global_auth_throttle_remote_key.cxx
        TEST http-auth/compile-fail-global-auth-throttle-remote-key
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_remote_key")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_form_key
        SOURCE http_auth_compile_fail_global_auth_throttle_form_key.cxx
        TEST http-auth/compile-fail-global-auth-throttle-form-key
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_form_key")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_query_key
        SOURCE http_auth_compile_fail_global_auth_throttle_query_key.cxx
        TEST http-auth/compile-fail-global-auth-throttle-query-key
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_query_key")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_bearer_key
        SOURCE http_auth_compile_fail_global_auth_throttle_bearer_key.cxx
        TEST http-auth/compile-fail-global-auth-throttle-bearer-key
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_bearer_key")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_too_many_requests
        SOURCE http_auth_compile_fail_global_auth_throttle_too_many_requests.cxx
        TEST http-auth/compile-fail-global-auth-throttle-too-many-requests
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_too_many_requests")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_middleware
        SOURCE http_auth_compile_fail_global_auth_throttle_middleware.cxx
        TEST http-auth/compile-fail-global-auth-throttle-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_basic_auth_middleware
        SOURCE http_auth_compile_fail_global_basic_auth_middleware.cxx
        TEST http-auth/compile-fail-global-basic-auth-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "basic_auth_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_bearer_auth_middleware
        SOURCE http_auth_compile_fail_global_bearer_auth_middleware.cxx
        TEST http-auth/compile-fail-global-bearer-auth-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "bearer_auth_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_claims
        SOURCE http_auth_compile_fail_global_jwt_claims.cxx
        TEST http-auth/compile-fail-global-jwt-claims
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "JwtClaims")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_options
        SOURCE http_auth_compile_fail_global_jwt_options.cxx
        TEST http-auth/compile-fail-global-jwt-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "JwtOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_options_from_config
        SOURCE http_auth_compile_fail_global_jwt_options_from_config.cxx
        TEST http-auth/compile-fail-global-jwt-options-from-config
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "jwt_options_from_config")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_sign
        SOURCE http_auth_compile_fail_global_jwt_sign.cxx
        TEST http-auth/compile-fail-global-jwt-sign
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "jwt_sign")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_decode
        SOURCE http_auth_compile_fail_global_jwt_decode.cxx
        TEST http-auth/compile-fail-global-jwt-decode
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "jwt_decode")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_middleware
        SOURCE http_auth_compile_fail_global_jwt_middleware.cxx
        TEST http-auth/compile-fail-global-jwt-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "jwt_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_algorithm
        SOURCE http_auth_compile_fail_global_password_hash_algorithm.cxx
        TEST http-auth/compile-fail-global-password-hash-algorithm
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "PasswordHashAlgorithm")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_options
        SOURCE http_auth_compile_fail_global_password_hash_options.cxx
        TEST http-auth/compile-fail-global-password-hash-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "PasswordHashOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_secrets
        SOURCE http_auth_compile_fail_global_password_hash_secrets.cxx
        TEST http-auth/compile-fail-global-password-hash-secrets
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "PasswordHashSecrets")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_resource_limits
        SOURCE http_auth_compile_fail_global_password_hash_resource_limits.cxx
        TEST http-auth/compile-fail-global-password-hash-resource-limits
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "PasswordHashResourceLimits")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_verify_result
        SOURCE http_auth_compile_fail_global_password_verify_result.cxx
        TEST http-auth/compile-fail-global-password-verify-result
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "PasswordVerifyResult")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_argon2id_available
        SOURCE http_auth_compile_fail_global_password_hash_argon2id_available.cxx
        TEST http-auth/compile-fail-global-password-hash-argon2id-available
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash_argon2id_available")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_configure_resource_limits
        SOURCE http_auth_compile_fail_global_password_hash_configure_resource_limits.cxx
        TEST http-auth/compile-fail-global-password-hash-configure-resource-limits
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash_configure_resource_limits")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_resource_limits_fn
        SOURCE http_auth_compile_fail_global_password_hash_resource_limits_fn.cxx
        TEST http-auth/compile-fail-global-password-hash-resource-limits-fn
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash_resource_limits")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_pbkdf2_sha256_password_hash_options
        SOURCE http_auth_compile_fail_global_pbkdf2_sha256_password_hash_options.cxx
        TEST http-auth/compile-fail-global-pbkdf2-sha256-password-hash-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "pbkdf2_sha256_password_hash_options")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_secrets_from_config
        SOURCE http_auth_compile_fail_global_password_hash_secrets_from_config.cxx
        TEST http-auth/compile-fail-global-password-hash-secrets-from-config
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash_secrets_from_config")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_with_salt
        SOURCE http_auth_compile_fail_global_password_hash_with_salt.cxx
        TEST http-auth/compile-fail-global-password-hash-with-salt
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash_with_salt")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash
        SOURCE http_auth_compile_fail_global_password_hash.cxx
        TEST http-auth/compile-fail-global-password-hash
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_verify
        SOURCE http_auth_compile_fail_global_password_verify.cxx
        TEST http-auth/compile-fail-global-password-verify
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_verify")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_needs_rehash
        SOURCE http_auth_compile_fail_global_password_needs_rehash.cxx
        TEST http-auth/compile-fail-global-password-needs-rehash
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_needs_rehash")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_cookie_signing_options
        SOURCE http_auth_compile_fail_global_cookie_signing_options.cxx
        TEST http-auth/compile-fail-global-cookie-signing-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "CookieSigningOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_cookie_signing_middleware
        SOURCE http_auth_compile_fail_global_cookie_signing_middleware.cxx
        TEST http-auth/compile-fail-global-cookie-signing-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "cookie_signing_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_sign_cookie
        SOURCE http_auth_compile_fail_global_sign_cookie.cxx
        TEST http-auth/compile-fail-global-sign-cookie
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "sign_cookie")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_verify_cookie
        SOURCE http_auth_compile_fail_global_verify_cookie.cxx
        TEST http-auth/compile-fail-global-verify-cookie
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "verify_cookie")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_cookie_signing_options_from_config
        SOURCE http_auth_compile_fail_global_cookie_signing_options_from_config.cxx
        TEST http-auth/compile-fail-global-cookie-signing-options-from-config
        LINK conflux_http_auth conflux_net_config conflux_options
        LABELS http compile-fail
        EXPECT "cookie_signing_options_from_config")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_csrf_options
        SOURCE http_auth_compile_fail_global_csrf_options.cxx
        TEST http-auth/compile-fail-global-csrf-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "CsrfOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_csrf_middleware
        SOURCE http_auth_compile_fail_global_csrf_middleware.cxx
        TEST http-auth/compile-fail-global-csrf-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "csrf_middleware")
endif()
