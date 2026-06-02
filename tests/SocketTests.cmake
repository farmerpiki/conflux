add_executable(conflux_resource_tests resource_limits_test.cxx)
target_link_libraries(conflux_resource_tests
    PRIVATE conflux_socket_io conflux_file_io_sync conflux_options conflux_direct_slot_pool Catch2::Catch2WithMain)

if(CONFLUX_ENABLE_RECV_BUNDLE)
    add_executable(recv_bundle_assert_probe recv_bundle_assert_probe.cxx)
    target_link_libraries(recv_bundle_assert_probe
        PRIVATE conflux_socket_io conflux_options)

    add_executable(conflux_recv_bundle_tests)
    target_sources(conflux_recv_bundle_tests
        PRIVATE
            recv_bundle_test.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            assert_probe_support.cxx)
    target_link_libraries(conflux_recv_bundle_tests
        PRIVATE conflux_socket_io conflux_options Catch2::Catch2WithMain)
    target_compile_definitions(conflux_recv_bundle_tests PRIVATE
        "ASSERT_PROBE_BIN=\"$<TARGET_FILE:recv_bundle_assert_probe>\"")
endif()

if(CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF)
    add_executable(incremental_buf_ring_assert_probe incremental_buf_ring_assert_probe.cxx)
    target_link_libraries(incremental_buf_ring_assert_probe
        PRIVATE conflux_socket_io conflux_options)

    add_executable(conflux_incremental_buf_ring_tests)
    target_sources(conflux_incremental_buf_ring_tests
        PRIVATE
            incremental_buf_ring_test.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            assert_probe_support.cxx)
    target_link_libraries(conflux_incremental_buf_ring_tests
        PRIVATE conflux_socket_io conflux_options Catch2::Catch2WithMain)
    target_compile_definitions(conflux_incremental_buf_ring_tests PRIVATE
        "ASSERT_PROBE_BIN=\"$<TARGET_FILE:incremental_buf_ring_assert_probe>\"")
endif()

add_executable(conflux_tcp_listener_tests tcp_listener_test.cxx)
target_link_libraries(conflux_tcp_listener_tests
    PRIVATE conflux_socket_io conflux_options Catch2::Catch2WithMain)

if(CONFLUX_ENABLE_RECV_BUNDLE)
    add_executable(conflux_recv_bundle_e2e_tests)
    target_sources(conflux_recv_bundle_e2e_tests
        PRIVATE
            recv_bundle_e2e_test.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            support.cxx
    )
    target_link_libraries(conflux_recv_bundle_e2e_tests
        PRIVATE conflux conflux_options Catch2::Catch2WithMain)
endif()
