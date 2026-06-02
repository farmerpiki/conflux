if(TARGET conflux_http_server AND CONFLUX_EXPERIMENTAL_SEND_ZC)
    add_executable(conflux_send_zc_lifecycle_tests send_zc_lifecycle_test.cxx)
    target_link_libraries(conflux_send_zc_lifecycle_tests
        PRIVATE conflux_http_server conflux_options Catch2::Catch2WithMain)
endif()
