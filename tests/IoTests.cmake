add_executable(conflux_cq_overflow_tests cq_overflow_test.cxx)
target_link_libraries(conflux_cq_overflow_tests
    PRIVATE
        conflux_uring
        conflux_options
        Catch2::Catch2WithMain
)

if(CONFLUX_EXPERIMENTAL_RING_GROWTH)
    add_executable(conflux_ring_resize_tests ring_resize_test.cxx)
    target_link_libraries(conflux_ring_resize_tests
        PRIVATE
            conflux_uring
            conflux_options
            Catch2::Catch2WithMain
    )
endif()

add_executable(conflux_uring_caps_tests caps_test.cxx)
target_link_libraries(conflux_uring_caps_tests
    PRIVATE
        conflux_uring
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_file_io_completion_tests file_io_completion_test.cxx)
target_link_libraries(conflux_file_io_completion_tests
    PRIVATE
        conflux_uring
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_file_io_fixed_tests file_io_fixed_test.cxx)
target_link_libraries(conflux_file_io_fixed_tests
    PRIVATE
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_file_io_pipe_tests file_io_pipe_test.cxx)
target_link_libraries(conflux_file_io_pipe_tests
    PRIVATE
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_file_io_xattr_tests file_io_xattr_test.cxx)
target_link_libraries(conflux_file_io_xattr_tests
    PRIVATE
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_poll_first_auto_tests poll_first_auto_test.cxx)
target_link_libraries(conflux_poll_first_auto_tests
    PRIVATE
        conflux_socket_io
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_direct_accept_sqe_tests direct_accept_sqe_test.cxx)
target_link_libraries(conflux_direct_accept_sqe_tests
    PRIVATE
        conflux_socket_io
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_file_io_tests file_io_test.cxx)
target_link_libraries(conflux_file_io_tests
    PRIVATE
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_file_io_sync_tests file_io_sync_test.cxx)
target_link_libraries(conflux_file_io_sync_tests
    PRIVATE
        conflux_file_io_sync
        conflux_file_map
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_dns_codec_tests dns_codec_test.cxx)
target_link_libraries(conflux_dns_codec_tests
    PRIVATE
        conflux_dns
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_dns_resolver_tests dns_resolver_test.cxx)
target_link_libraries(conflux_dns_resolver_tests
    PRIVATE
        conflux_dns
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_udp_tests udp_test.cxx)
target_link_libraries(conflux_udp_tests
    PRIVATE
        conflux_socket_io
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_socket_task_ring_tests socket_task_ring_test.cxx)
target_link_libraries(conflux_socket_task_ring_tests
    PRIVATE
        conflux_socket_io
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)
