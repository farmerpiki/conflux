if(TARGET conflux_http_router_dispatch)
    conflux_add_compile_fail_test(
        TARGET conflux_http_router_dispatch_compile_fail_global_deferred_task_options
        SOURCE http_router_dispatch_compile_fail_global_deferred_task_options.cxx
        TEST http-router-dispatch/compile-fail-global-deferred-task-options
        LINK conflux_http_router_dispatch conflux_options
        LABELS http compile-fail
        EXPECT "DeferredTaskOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_dispatch_compile_fail_global_dispatch_sync_routes
        SOURCE http_router_dispatch_compile_fail_global_dispatch_sync_routes.cxx
        TEST http-router-dispatch/compile-fail-global-dispatch-sync-routes
        LINK conflux_http_router_dispatch conflux_options
        LABELS http compile-fail
        EXPECT "dispatch_sync_routes")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_dispatch_compile_fail_global_router_run_async_http_task
        SOURCE http_router_dispatch_compile_fail_global_router_run_async_http_task.cxx
        TEST http-router-dispatch/compile-fail-global-router-run-async-http-task
        LINK conflux_http_router_dispatch conflux_options
        LABELS http compile-fail
        EXPECT "router_run_async_http_task")
endif()
