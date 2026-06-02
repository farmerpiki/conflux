add_library(conflux_work_api_snapshot OBJECT work_api_snapshot.cxx)
target_link_libraries(conflux_work_api_snapshot PRIVATE conflux conflux_options)

add_executable(conflux_work_tests work_test.cxx)
target_link_libraries(conflux_work_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_root_tests work_root_test.cxx)
target_link_libraries(conflux_work_root_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_race_tests work_race_test.cxx)
target_link_libraries(conflux_work_race_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_tests work_carrier_test.cxx)
target_link_libraries(conflux_work_carrier_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase2_tests work_carrier_phase2_test.cxx)
target_link_libraries(conflux_work_carrier_phase2_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase3_tests work_carrier_phase3_test.cxx)
target_link_libraries(conflux_work_carrier_phase3_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase4_tests work_carrier_phase4_test.cxx)
target_link_libraries(conflux_work_carrier_phase4_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase5_tests work_carrier_phase5_test.cxx)
target_link_libraries(conflux_work_carrier_phase5_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase6_tests work_carrier_phase6_test.cxx)
target_link_libraries(conflux_work_carrier_phase6_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_streams_tests work_carrier_streams_test.cxx)
target_link_libraries(conflux_work_carrier_streams_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_timer_tests work_carrier_timer_test.cxx)
target_link_libraries(conflux_work_carrier_timer_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_uring_flow_tests uring_flow_test.cxx)
target_link_libraries(conflux_uring_flow_tests
    PRIVATE
        conflux_uring
        conflux_options
        Catch2::Catch2WithMain
)
target_compile_definitions(conflux_uring_flow_tests PRIVATE CONFLUX_TESTING)

add_executable(conflux_owned_path_flow_tests owned_path_flow_test.cxx)
target_link_libraries(conflux_owned_path_flow_tests
    PRIVATE
        conflux_uring
        conflux_options
        Catch2::Catch2WithMain
)
target_compile_definitions(conflux_owned_path_flow_tests PRIVATE CONFLUX_TESTING)

add_executable(conflux_direct_slot_pool_tests direct_slot_pool_test.cxx)
target_link_libraries(conflux_direct_slot_pool_tests
    PRIVATE
        conflux_types
        conflux_options
        conflux_direct_slot_pool
        Catch2::Catch2WithMain
)
target_include_directories(conflux_direct_slot_pool_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/net")

conflux_add_compile_fail_test(
    TARGET conflux_direct_slot_pool_compile_fail_global_pool
    SOURCE direct_slot_pool_compile_fail_global_pool.cxx
    TEST direct-slot-pool/compile-fail-global-pool
    LINK conflux_direct_slot_pool conflux_options
    LABELS compile-fail
    EXPECT "DirectSlotPool")

conflux_add_compile_fail_test(
    TARGET conflux_direct_slot_pool_compile_fail_global_state
    SOURCE direct_slot_pool_compile_fail_global_state.cxx
    TEST direct-slot-pool/compile-fail-global-state
    LINK conflux_direct_slot_pool conflux_options
    LABELS compile-fail
    EXPECT "DirectSlotState")
