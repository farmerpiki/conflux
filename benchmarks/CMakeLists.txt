add_executable(conflux_benchmarks router_bench.cxx)
target_link_libraries(conflux_benchmarks PRIVATE conflux conflux_options)

add_executable(conflux_work_benchmarks work_bench.cxx)
target_link_libraries(conflux_work_benchmarks PRIVATE conflux_work conflux_options)

add_executable(conflux_file_copy_coro_bench file_copy_coro_bench.cxx)
target_link_libraries(conflux_file_copy_coro_bench
    PRIVATE conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)

add_executable(conflux_tcp_increment_coro_bench tcp_increment_coro_bench.cxx)
target_link_libraries(conflux_tcp_increment_coro_bench
    PRIVATE conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)

add_executable(conflux_tcp_parallel_coro_bench tcp_parallel_coro_bench.cxx)
target_link_libraries(conflux_tcp_parallel_coro_bench
    PRIVATE conflux conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)

if(CONFLUX_HAS_TLS STREQUAL "true")
    add_executable(conflux_tls_tcp_increment_coro_bench tls_tcp_increment_coro_bench.cxx)
    target_link_libraries(conflux_tls_tcp_increment_coro_bench
        PRIVATE conflux conflux_file_io conflux_work conflux_options
                PkgConfig::LIBURING OpenSSL::SSL OpenSSL::Crypto)
endif()

if(CONFLUX_HAS_DB STREQUAL "true")
    add_executable(conflux_db_coro_bench db_coro_bench.cxx)
    target_link_libraries(conflux_db_coro_bench
        PRIVATE conflux_db conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)
endif()

add_executable(conflux_json_bench json_bench.cxx)
target_link_libraries(conflux_json_bench PRIVATE conflux conflux_options)

add_executable(conflux_template_bench template_bench.cxx)
target_link_libraries(conflux_template_bench PRIVATE conflux conflux_options)
