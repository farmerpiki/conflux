if(TARGET conflux_http_server)
    conflux_add_compile_fail_test(
        TARGET conflux_http_server_compile_fail_global_http_server
        SOURCE http_server_compile_fail_global_http_server.cxx
        TEST http-server/compile-fail-global-http-server
        LINK conflux_http_server conflux_options
        LABELS http compile-fail
        EXPECT "HttpServer")
endif()
