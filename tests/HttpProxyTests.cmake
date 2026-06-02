if(TARGET conflux_http_proxy)
    conflux_add_compile_fail_test(
        TARGET conflux_http_proxy_compile_fail_global_proxy_options
        SOURCE http_proxy_compile_fail_global_proxy_options.cxx
        TEST http-proxy/compile-fail-global-proxy-options
        LINK conflux_http_proxy conflux_options
        LABELS http compile-fail
        EXPECT "ProxyOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_proxy_compile_fail_global_blocking_proxy
        SOURCE http_proxy_compile_fail_global_blocking_proxy.cxx
        TEST http-proxy/compile-fail-global-blocking-proxy
        LINK conflux_http_proxy conflux_options
        LABELS http compile-fail
        EXPECT "blocking_proxy")

    conflux_add_compile_fail_test(
        TARGET conflux_http_proxy_compile_fail_global_async_proxy
        SOURCE http_proxy_compile_fail_global_async_proxy.cxx
        TEST http-proxy/compile-fail-global-async-proxy
        LINK conflux_http_proxy conflux_options
        LABELS http compile-fail
        EXPECT "async_proxy")
endif()
