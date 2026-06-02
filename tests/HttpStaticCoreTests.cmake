if(TARGET conflux_http_static_core)
    conflux_add_compile_fail_test(
        TARGET conflux_http_static_core_compile_fail_global_static_request
        SOURCE http_static_core_compile_fail_global_static_request.cxx
        TEST http-static-core/compile-fail-global-static-request
        LINK conflux_http_static_core conflux_options
        LABELS http compile-fail
        EXPECT "StaticRequest")

    conflux_add_compile_fail_test(
        TARGET conflux_http_static_core_compile_fail_global_static_cache_store
        SOURCE http_static_core_compile_fail_global_static_cache_store.cxx
        TEST http-static-core/compile-fail-global-static-cache-store
        LINK conflux_http_static_core conflux_options
        LABELS http compile-fail
        EXPECT "StaticCacheStore")

    conflux_add_compile_fail_test(
        TARGET conflux_http_static_core_compile_fail_global_normalize_static_path
        SOURCE http_static_core_compile_fail_global_normalize_static_path.cxx
        TEST http-static-core/compile-fail-global-normalize-static-path
        LINK conflux_http_static_core conflux_options
        LABELS http compile-fail
        EXPECT "normalize_static_path")
endif()
