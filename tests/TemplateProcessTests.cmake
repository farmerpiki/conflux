add_executable(conflux_template_tests template_test.cxx)
target_link_libraries(
    conflux_template_tests
    PRIVATE conflux_template conflux_json conflux_types conflux_options Catch2::Catch2WithMain)

add_executable(conflux_process_tests process_test.cxx)
target_link_libraries(conflux_process_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)
