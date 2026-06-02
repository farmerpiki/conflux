if(TARGET conflux_net_cancel)
    conflux_add_compile_fail_test(
        TARGET conflux_net_cancel_compile_fail_global_active_task_cancel_relay
        SOURCE net_cancel_compile_fail_global_active_task_cancel_relay.cxx
        TEST net-cancel/compile-fail-global-active-task-cancel-relay
        LINK conflux_net_cancel conflux_options
        LABELS compile-fail
        EXPECT "ActiveTaskCancelRelay")
endif()

if(TARGET conflux_net_io_buffer)
    conflux_add_compile_fail_test(
        TARGET conflux_net_io_buffer_compile_fail_global_io_buffer
        SOURCE net_io_buffer_compile_fail_global_io_buffer.cxx
        TEST net-io-buffer/compile-fail-global-io-buffer
        LINK conflux_net_io_buffer conflux_options
        LABELS compile-fail
        EXPECT "IoBuffer")

    conflux_add_compile_fail_test(
        TARGET conflux_net_io_buffer_compile_fail_global_io_plan
        SOURCE net_io_buffer_compile_fail_global_io_plan.cxx
        TEST net-io-buffer/compile-fail-global-io-plan
        LINK conflux_net_io_buffer conflux_options
        LABELS compile-fail
        EXPECT "IoPlan")
endif()

if(TARGET conflux_http2)
    conflux_add_compile_fail_test(
        TARGET conflux_http2_compile_fail_global_configure_alpn
        SOURCE http2_compile_fail_global_configure_alpn.cxx
        TEST http2/compile-fail-global-configure-alpn
        LINK conflux_http2 conflux_options
        LABELS http compile-fail
        EXPECT "http2_configure_alpn")

    conflux_add_compile_fail_test(
        TARGET conflux_http2_compile_fail_global_negotiated
        SOURCE http2_compile_fail_global_negotiated.cxx
        TEST http2/compile-fail-global-negotiated
        LINK conflux_http2 conflux_options
        LABELS http compile-fail
        EXPECT "http2_negotiated")
endif()

if(TARGET conflux_http3)
    conflux_add_compile_fail_test(
        TARGET conflux_http3_compile_fail_global_alt_svc
        SOURCE http3_compile_fail_global_alt_svc.cxx
        TEST http3/compile-fail-global-alt-svc
        LINK conflux_http3 conflux_options
        LABELS http compile-fail
        EXPECT "http3_alt_svc_value")

    conflux_add_compile_fail_test(
        TARGET conflux_http3_compile_fail_global_listener
        SOURCE http3_compile_fail_global_listener.cxx
        TEST http3/compile-fail-global-listener
        LINK conflux_http3 conflux_options
        LABELS http compile-fail
        EXPECT "Http3Listener")
endif()
