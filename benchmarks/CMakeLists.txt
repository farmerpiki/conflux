add_library(conflux_bench_release_opts INTERFACE)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(conflux_bench_release_opts INTERFACE
        -ffunction-sections -fdata-sections -falign-functions=64)
    target_link_options(conflux_bench_release_opts INTERFACE -Wl,--gc-sections)
endif()

add_library(bench_common STATIC)
target_sources(bench_common
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES bench_common.cxx
)
target_link_libraries(bench_common PUBLIC conflux_options)

add_executable(conflux_benchmarks router_bench.cxx)
target_link_libraries(conflux_benchmarks PRIVATE conflux conflux_options)

set(CONFLUX_BENCH_SPIN "" CACHE STRING "Override spin_before_park in work_bench (e.g. 64, 128)")

add_executable(conflux_work_benchmarks work_bench.cxx)
target_link_libraries(conflux_work_benchmarks PRIVATE conflux_work conflux_options conflux_bench_release_opts)
if(NOT CONFLUX_BENCH_SPIN STREQUAL "")
    target_compile_definitions(conflux_work_benchmarks PRIVATE "CONFLUX_BENCH_SPIN_BEFORE_PARK=${CONFLUX_BENCH_SPIN}")
endif()

add_executable(conflux_work_compile_bench work_compile_bench.cxx)
target_link_libraries(conflux_work_compile_bench PRIVATE bench_common conflux_work conflux_options)

add_executable(conflux_file_copy_coro_bench file_copy_coro_bench.cxx)
target_link_libraries(conflux_file_copy_coro_bench
    PRIVATE bench_common conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)

add_executable(conflux_tcp_increment_coro_bench tcp_increment_coro_bench.cxx)
target_link_libraries(conflux_tcp_increment_coro_bench
    PRIVATE bench_common conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)

# TODO: co_spawn not yet implemented — re-enable when available
# add_executable(conflux_tcp_parallel_coro_bench tcp_parallel_coro_bench.cxx)
# target_link_libraries(conflux_tcp_parallel_coro_bench
#     PRIVATE conflux conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)

if(CONFLUX_HAS_TLS STREQUAL "true")
    add_executable(conflux_tls_tcp_increment_coro_bench tls_tcp_increment_coro_bench.cxx)
    target_link_libraries(conflux_tls_tcp_increment_coro_bench
        PRIVATE bench_common conflux conflux_file_io conflux_work conflux_options
                PkgConfig::LIBURING OpenSSL::SSL OpenSSL::Crypto)
endif()

if(CONFLUX_HAS_DB STREQUAL "true")
    add_executable(conflux_db_coro_bench db_coro_bench.cxx)
    target_link_libraries(conflux_db_coro_bench
        PRIVATE bench_common conflux_db conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)

    add_executable(conflux_db_params_bench db_params_bench.cxx)
    target_link_libraries(conflux_db_params_bench
        PRIVATE bench_common conflux_db conflux_options)

    add_executable(conflux_db_pipeline_bench db_pipeline_bench.cxx)
    target_link_libraries(conflux_db_pipeline_bench
        PRIVATE bench_common conflux_db conflux_file_io conflux_work conflux_options PkgConfig::LIBURING)
endif()

add_executable(conflux_json_bench json_bench.cxx)
target_link_libraries(conflux_json_bench PRIVATE bench_common conflux conflux_options)
if(CONFLUX_JSON_AVX2)
    target_compile_options(conflux_json_bench PRIVATE -mavx2 -mbmi2)
endif()

add_executable(conflux_template_bench template_bench.cxx)
target_link_libraries(conflux_template_bench PRIVATE conflux conflux_options)

add_executable(conflux_task_creation_bench task_creation_bench.cxx)
target_link_libraries(conflux_task_creation_bench PRIVATE bench_common conflux_work conflux_options)

add_executable(conflux_task_chain_composition_bench task_chain_composition_bench.cxx)
target_link_libraries(conflux_task_chain_composition_bench PRIVATE bench_common conflux_work conflux_options)

add_executable(conflux_task_cancellation_bench task_cancellation_bench.cxx)
target_link_libraries(conflux_task_cancellation_bench PRIVATE bench_common conflux_work conflux_options)

add_executable(conflux_workpool_enqueue_dequeue_bench workpool_enqueue_dequeue_bench.cxx)
target_link_libraries(conflux_workpool_enqueue_dequeue_bench PRIVATE bench_common conflux_work conflux_options)

add_executable(conflux_join_all_N_bench join_all_N_bench.cxx)
target_link_libraries(conflux_join_all_N_bench PRIVATE bench_common conflux_work conflux_options)

# Meta-target: build all benches that support --bench-info recording.
set(_record_targets
    conflux_benchmarks
    conflux_work_benchmarks
    conflux_file_copy_coro_bench
    conflux_tcp_increment_coro_bench
    # conflux_tcp_parallel_coro_bench
    conflux_json_bench
    conflux_template_bench
    conflux_task_creation_bench
    conflux_task_chain_composition_bench
    conflux_task_cancellation_bench
    conflux_workpool_enqueue_dequeue_bench
    conflux_join_all_N_bench
)
if(CONFLUX_HAS_TLS STREQUAL "true")
    list(APPEND _record_targets conflux_tls_tcp_increment_coro_bench)
endif()
if(CONFLUX_HAS_DB STREQUAL "true")
    list(APPEND _record_targets conflux_db_coro_bench conflux_db_params_bench conflux_db_pipeline_bench)
endif()
add_custom_target(conflux_record_benches DEPENDS ${_record_targets})
