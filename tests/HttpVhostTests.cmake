if(TARGET conflux_http_vhost)
    conflux_add_compile_fail_test(
        TARGET conflux_http_vhost_compile_fail_global_vhost_router
        SOURCE http_vhost_compile_fail_global_vhost_router.cxx
        TEST http-vhost/compile-fail-global-vhost-router
        LINK conflux_http_vhost conflux_options
        LABELS http compile-fail
        EXPECT "VHostRouter")
endif()
