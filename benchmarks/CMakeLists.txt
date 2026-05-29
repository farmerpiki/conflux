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
target_link_libraries(bench_common PUBLIC conflux_types conflux_options)

if(TARGET conflux_file_io AND TARGET conflux_work)
    add_library(bench_io_common STATIC)
    target_sources(bench_io_common
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES bench_io_common.cxx
    )
    target_link_libraries(bench_io_common PUBLIC
        bench_common conflux_file_io conflux_work conflux_options)
endif()

set_property(GLOBAL PROPERTY CONFLUX_RECORD_BENCH_TARGETS "")
function(conflux_add_recordable_benchmark target)
    add_executable(${target} ${ARGN})
    set_property(GLOBAL APPEND PROPERTY CONFLUX_RECORD_BENCH_TARGETS ${target})
endfunction()

if(TARGET conflux)
    conflux_add_recordable_benchmark(conflux_benchmarks router_bench.cxx)
    target_link_libraries(conflux_benchmarks PRIVATE bench_common conflux conflux_options)
endif()

set(CONFLUX_BENCH_SPIN "" CACHE STRING "Override spin_before_park in work_bench (e.g. 64, 128)")

if(TARGET conflux_work)
    conflux_add_recordable_benchmark(conflux_work_benchmarks work_bench.cxx)
    target_link_libraries(conflux_work_benchmarks PRIVATE conflux_work conflux_options conflux_bench_release_opts)
    if(NOT CONFLUX_BENCH_SPIN STREQUAL "")
        target_compile_definitions(conflux_work_benchmarks PRIVATE "CONFLUX_BENCH_SPIN_BEFORE_PARK=${CONFLUX_BENCH_SPIN}")
    endif()
endif()

if(TARGET conflux_work)
    add_executable(conflux_work_compile_bench work_compile_bench.cxx)
    target_link_libraries(conflux_work_compile_bench PRIVATE bench_common conflux_work conflux_options)
endif()

if(TARGET conflux_net_http)
    add_executable(conflux_import_http_probe import_http_probe.cxx)
    target_link_libraries(conflux_import_http_probe PRIVATE conflux_net_http conflux_options)
endif()

if(TARGET conflux)
    add_executable(conflux_import_umbrella_probe import_conflux_probe.cxx)
    target_link_libraries(conflux_import_umbrella_probe PRIVATE conflux conflux_options)
endif()

if(TARGET bench_io_common)
    conflux_add_recordable_benchmark(conflux_file_copy_coro_bench file_copy_coro_bench.cxx)
    target_link_libraries(conflux_file_copy_coro_bench
        PRIVATE bench_common conflux_file_io conflux_work conflux_options)
endif()

if(TARGET conflux_uring)
    conflux_add_recordable_benchmark(conflux_uring_completion_bench uring_completion_bench.cxx)
    target_link_libraries(conflux_uring_completion_bench
        PRIVATE bench_common conflux_uring conflux_options)
endif()

if(TARGET conflux_uring AND TARGET conflux_socket_io AND TARGET conflux_direct_slot_pool)
    conflux_add_recordable_benchmark(conflux_kernel_state_synthetic_bench kernel_state_synthetic_bench.cxx)
    target_link_libraries(conflux_kernel_state_synthetic_bench
        PRIVATE bench_common conflux_uring conflux_socket_io conflux_direct_slot_pool conflux_options)
endif()

if(TARGET conflux_uring_timeout AND TARGET conflux_uring AND TARGET conflux_work AND TARGET bench_io_common)
    conflux_add_recordable_benchmark(conflux_uring_timeout_bench uring_timeout_bench.cxx)
    target_link_libraries(conflux_uring_timeout_bench
        PRIVATE bench_io_common conflux_uring_timeout conflux_uring conflux_work conflux_options)
endif()

if(TARGET conflux_uring AND TARGET conflux_work)
    conflux_add_recordable_benchmark(conflux_synthetic_cqe_coro_bench synthetic_cqe_coro_bench.cxx)
    target_link_libraries(conflux_synthetic_cqe_coro_bench
        PRIVATE bench_common conflux_uring conflux_work conflux_options)
endif()

if(TARGET bench_io_common)
    conflux_add_recordable_benchmark(conflux_file_io_sqe_storm_bench file_io_sqe_storm_bench.cxx)
    target_link_libraries(conflux_file_io_sqe_storm_bench
        PRIVATE bench_io_common)

    conflux_add_recordable_benchmark(conflux_file_io_fixed_bench file_io_fixed_bench.cxx)
    target_link_libraries(conflux_file_io_fixed_bench
        PRIVATE bench_io_common)

    conflux_add_recordable_benchmark(conflux_storage_read_bench storage_read_bench.cxx)
    target_link_libraries(conflux_storage_read_bench
        PRIVATE bench_io_common)
endif()

if(TARGET bench_io_common AND TARGET conflux_socket_io)
    conflux_add_recordable_benchmark(conflux_tcp_increment_coro_bench tcp_increment_coro_bench.cxx)
    target_link_libraries(conflux_tcp_increment_coro_bench
        PRIVATE bench_common conflux_file_io conflux_socket_io conflux_work conflux_options)
endif()


if(CONFLUX_HAS_TLS STREQUAL "true" AND TARGET bench_io_common AND TARGET conflux)
    conflux_add_recordable_benchmark(conflux_tls_tcp_increment_coro_bench tls_tcp_increment_coro_bench.cxx)
    target_link_libraries(conflux_tls_tcp_increment_coro_bench
        PRIVATE bench_common conflux conflux_file_io conflux_work conflux_options
                OpenSSL::SSL OpenSSL::Crypto)
endif()

if(CONFLUX_HAS_TLS STREQUAL "true")
    conflux_add_recordable_benchmark(conflux_tls_mem_bio_bench tls_mem_bio_bench.cxx)
    target_link_libraries(conflux_tls_mem_bio_bench
        PRIVATE bench_common conflux_options OpenSSL::SSL OpenSSL::Crypto)
endif()

if(CONFLUX_HAS_DB STREQUAL "true")
    if(TARGET bench_io_common)
        conflux_add_recordable_benchmark(conflux_db_coro_bench db_coro_bench.cxx)
        target_link_libraries(conflux_db_coro_bench
            PRIVATE bench_common conflux_db conflux_file_io conflux_work conflux_options)
    endif()

    conflux_add_recordable_benchmark(conflux_db_params_bench db_params_bench.cxx)
    target_link_libraries(conflux_db_params_bench
        PRIVATE bench_common conflux_db conflux_options)

    if(TARGET bench_io_common)
        conflux_add_recordable_benchmark(conflux_db_pipeline_bench db_pipeline_bench.cxx)
        target_link_libraries(conflux_db_pipeline_bench
            PRIVATE bench_common conflux_db conflux_file_io conflux_work conflux_options)
    endif()

    conflux_add_recordable_benchmark(conflux_db_protocol_synthetic_bench db_protocol_synthetic_bench.cxx)
    target_link_libraries(conflux_db_protocol_synthetic_bench
        PRIVATE bench_common conflux_db conflux_options)
endif()

if(TARGET conflux_json AND TARGET bench_io_common AND TARGET conflux_socket_io)
    conflux_add_recordable_benchmark(conflux_json_bench json_bench.cxx)
    target_link_libraries(conflux_json_bench
        PRIVATE bench_common conflux_file_io conflux_socket_io conflux_work conflux_json conflux_options)
    if(CONFLUX_JSON_AVX2)
        target_compile_options(conflux_json_bench PRIVATE -mavx2 -mbmi2)
    endif()
endif()

if(TARGET conflux_json)
    conflux_add_recordable_benchmark(conflux_json_storage_bench json_storage_bench.cxx)
    target_link_libraries(conflux_json_storage_bench PRIVATE bench_common conflux_json conflux_options)

    conflux_add_recordable_benchmark(conflux_json_direct_struct_bench json_direct_struct_bench.cxx)
    target_link_libraries(conflux_json_direct_struct_bench PRIVATE bench_common conflux_json conflux_options)
endif()

if(CONFLUX_JSON_REFLECT AND TARGET conflux_json_reflect_provider)
    conflux_add_recordable_benchmark(conflux_json_reflect_bench json_reflect_bench.cxx)
    target_link_libraries(conflux_json_reflect_bench
        PRIVATE bench_common conflux_json_reflect_provider conflux_json_reflect conflux_json conflux_options)
    target_compile_options(conflux_json_reflect_bench PRIVATE -freflection)
endif()

if(TARGET conflux)
    conflux_add_recordable_benchmark(conflux_crypto_bench crypto_bench.cxx)
    target_link_libraries(conflux_crypto_bench PRIVATE bench_common conflux conflux_options)
endif()

if(_conflux_cpu_feature_probes_runtime AND TARGET conflux_crypto)
    conflux_add_recordable_benchmark(conflux_cpu_dispatch_impl_bench cpu_dispatch_impl_bench.cxx)
    target_link_libraries(
        conflux_cpu_dispatch_impl_bench
        PRIVATE bench_common conflux_crypto conflux_utils conflux_cpu_features conflux_options)
    if(TARGET conflux_json)
        target_link_libraries(conflux_cpu_dispatch_impl_bench PRIVATE conflux_json)
    endif()
    if(NOT _conflux_simd_backend STREQUAL "OFF")
        target_compile_definitions(conflux_cpu_dispatch_impl_bench PRIVATE CONFLUX_BENCH_HAS_STDSIMD=1)
        target_link_libraries(conflux_cpu_dispatch_impl_bench PRIVATE conflux_simd_runtime)
    endif()
    if(_conflux_simd_backend STREQUAL "STDX")
        add_library(conflux_bench_simd_stdx_sse2 OBJECT ../src/simd_stdx.cxx)
        target_compile_features(conflux_bench_simd_stdx_sse2 PRIVATE cxx_std_23)
        target_link_libraries(conflux_bench_simd_stdx_sse2 PRIVATE conflux_options)
        target_compile_options(conflux_bench_simd_stdx_sse2 PRIVATE "-msse2")
        target_compile_definitions(conflux_bench_simd_stdx_sse2 PRIVATE
            CONFLUX_ASCII_LOWER_INPLACE_STDSIMD=conflux_ascii_lower_inplace_stdx_sse2
            CONFLUX_CONSTANT_TIME_EQ_STDSIMD=conflux_constant_time_eq_stdx_sse2
            CONFLUX_URL_SCAN_PLAIN_RUN_STDSIMD=conflux_url_scan_plain_run_stdx_sse2
            CONFLUX_WS_UNMASK_STDSIMD=conflux_ws_unmask_stdx_sse2)

        add_library(conflux_bench_simd_stdx_avx2 OBJECT ../src/simd_stdx.cxx)
        target_compile_features(conflux_bench_simd_stdx_avx2 PRIVATE cxx_std_23)
        target_link_libraries(conflux_bench_simd_stdx_avx2 PRIVATE conflux_options)
        target_compile_options(conflux_bench_simd_stdx_avx2 PRIVATE "-mavx2")
        target_compile_definitions(conflux_bench_simd_stdx_avx2 PRIVATE
            CONFLUX_ASCII_LOWER_INPLACE_STDSIMD=conflux_ascii_lower_inplace_stdx_avx2
            CONFLUX_CONSTANT_TIME_EQ_STDSIMD=conflux_constant_time_eq_stdx_avx2
            CONFLUX_URL_SCAN_PLAIN_RUN_STDSIMD=conflux_url_scan_plain_run_stdx_avx2
            CONFLUX_WS_UNMASK_STDSIMD=conflux_ws_unmask_stdx_avx2)

        target_compile_definitions(conflux_cpu_dispatch_impl_bench PRIVATE CONFLUX_BENCH_HAS_STDX_VARIANTS=1)
        target_sources(conflux_cpu_dispatch_impl_bench PRIVATE
            $<TARGET_OBJECTS:conflux_bench_simd_stdx_sse2>
            $<TARGET_OBJECTS:conflux_bench_simd_stdx_avx2>)
    endif()
    if(CONFLUX_HAS_SIMD_STD26)
        set(_conflux_bench_std26_options_sse2 "-msse2")
        set(_conflux_bench_std26_options_avx2 "-mavx2")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
                AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16
                AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
            list(APPEND _conflux_bench_std26_options_sse2 "-fno-lto")
            list(APPEND _conflux_bench_std26_options_avx2 "-fno-lto")
        endif()

        add_library(conflux_bench_simd_std26_sse2 OBJECT ../src/simd_std26.cxx)
        set_target_properties(conflux_bench_simd_std26_sse2 PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
        target_compile_features(conflux_bench_simd_std26_sse2 PRIVATE cxx_std_26)
        target_compile_options(conflux_bench_simd_std26_sse2 PRIVATE ${_conflux_bench_std26_options_sse2})
        target_compile_definitions(conflux_bench_simd_std26_sse2 PRIVATE
            CONFLUX_ASCII_LOWER_INPLACE_STDSIMD=conflux_ascii_lower_inplace_std26_sse2
            CONFLUX_CONSTANT_TIME_EQ_STDSIMD=conflux_constant_time_eq_std26_sse2
            CONFLUX_URL_SCAN_PLAIN_RUN_STDSIMD=conflux_url_scan_plain_run_std26_sse2
            CONFLUX_WS_UNMASK_STDSIMD=conflux_ws_unmask_std26_sse2)

        add_library(conflux_bench_simd_std26_avx2 OBJECT ../src/simd_std26.cxx)
        set_target_properties(conflux_bench_simd_std26_avx2 PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
        target_compile_features(conflux_bench_simd_std26_avx2 PRIVATE cxx_std_26)
        target_compile_options(conflux_bench_simd_std26_avx2 PRIVATE ${_conflux_bench_std26_options_avx2})
        target_compile_definitions(conflux_bench_simd_std26_avx2 PRIVATE
            CONFLUX_ASCII_LOWER_INPLACE_STDSIMD=conflux_ascii_lower_inplace_std26_avx2
            CONFLUX_CONSTANT_TIME_EQ_STDSIMD=conflux_constant_time_eq_std26_avx2
            CONFLUX_URL_SCAN_PLAIN_RUN_STDSIMD=conflux_url_scan_plain_run_std26_avx2
            CONFLUX_WS_UNMASK_STDSIMD=conflux_ws_unmask_std26_avx2)

        target_compile_definitions(conflux_cpu_dispatch_impl_bench PRIVATE CONFLUX_BENCH_HAS_STD26_VARIANTS=1)
        target_sources(conflux_cpu_dispatch_impl_bench PRIVATE
            $<TARGET_OBJECTS:conflux_bench_simd_std26_sse2>
            $<TARGET_OBJECTS:conflux_bench_simd_std26_avx2>)
        unset(_conflux_bench_std26_options_sse2)
        unset(_conflux_bench_std26_options_avx2)
    endif()
    if(NOT _conflux_simd_backend STREQUAL "OFF" AND TARGET conflux_json)
        target_compile_definitions(conflux_cpu_dispatch_impl_bench PRIVATE CONFLUX_BENCH_HAS_JSON_STDSIMD=1)
    endif()
    if((CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64") AND TARGET conflux_json)
        add_library(conflux_bench_json_intrin_sse2 OBJECT ../src/json_simd_intrinsics.cxx)
        target_compile_options(conflux_bench_json_intrin_sse2 PRIVATE "-msse2")
        target_compile_definitions(conflux_bench_json_intrin_sse2 PRIVATE
            CONFLUX_JSON_INTRIN_AVX2=0
            CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_INTRIN=conflux_json_scan_str_until_special_intrin_sse2
            CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_INTRIN=conflux_json_scan_dump_safe_run_intrin_sse2)

        add_library(conflux_bench_json_intrin_avx2 OBJECT ../src/json_simd_intrinsics.cxx)
        target_compile_options(conflux_bench_json_intrin_avx2 PRIVATE "-mavx2")
        target_compile_definitions(conflux_bench_json_intrin_avx2 PRIVATE
            CONFLUX_JSON_INTRIN_AVX2=1
            CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_INTRIN=conflux_json_scan_str_until_special_intrin_avx2
            CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_INTRIN=conflux_json_scan_dump_safe_run_intrin_avx2)

        target_compile_definitions(conflux_cpu_dispatch_impl_bench PRIVATE CONFLUX_BENCH_HAS_JSON_INTRIN_VARIANTS=1)
        target_sources(conflux_cpu_dispatch_impl_bench PRIVATE
            $<TARGET_OBJECTS:conflux_bench_json_intrin_sse2>
            $<TARGET_OBJECTS:conflux_bench_json_intrin_avx2>)
    endif()
    if(_conflux_simd_backend STREQUAL "STDX" AND TARGET conflux_json)
        add_library(conflux_bench_json_simd_stdx_sse2 OBJECT ../src/json_simd_stdx.cxx)
        target_compile_features(conflux_bench_json_simd_stdx_sse2 PRIVATE cxx_std_23)
        target_link_libraries(conflux_bench_json_simd_stdx_sse2 PRIVATE conflux_options)
        target_compile_options(conflux_bench_json_simd_stdx_sse2 PRIVATE "-msse2")
        target_compile_definitions(conflux_bench_json_simd_stdx_sse2 PRIVATE
            CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD=conflux_json_scan_str_until_special_stdx_sse2
            CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD=conflux_json_scan_dump_safe_run_stdx_sse2)

        add_library(conflux_bench_json_simd_stdx_avx2 OBJECT ../src/json_simd_stdx.cxx)
        target_compile_features(conflux_bench_json_simd_stdx_avx2 PRIVATE cxx_std_23)
        target_link_libraries(conflux_bench_json_simd_stdx_avx2 PRIVATE conflux_options)
        target_compile_options(conflux_bench_json_simd_stdx_avx2 PRIVATE "-mavx2")
        target_compile_definitions(conflux_bench_json_simd_stdx_avx2 PRIVATE
            CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD=conflux_json_scan_str_until_special_stdx_avx2
            CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD=conflux_json_scan_dump_safe_run_stdx_avx2)

        target_compile_definitions(conflux_cpu_dispatch_impl_bench PRIVATE CONFLUX_BENCH_HAS_JSON_STDX_VARIANTS=1)
        target_sources(conflux_cpu_dispatch_impl_bench PRIVATE
            $<TARGET_OBJECTS:conflux_bench_json_simd_stdx_sse2>
            $<TARGET_OBJECTS:conflux_bench_json_simd_stdx_avx2>)
    endif()
    if(CONFLUX_HAS_SIMD_STD26 AND TARGET conflux_json)
        set(_conflux_bench_json_std26_options_sse2 "-msse2")
        set(_conflux_bench_json_std26_options_avx2 "-mavx2")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
                AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16
                AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
            list(APPEND _conflux_bench_json_std26_options_sse2 "-fno-lto")
            list(APPEND _conflux_bench_json_std26_options_avx2 "-fno-lto")
        endif()

        add_library(conflux_bench_json_simd_std26_sse2 OBJECT ../src/json_simd_std26.cxx)
        set_target_properties(conflux_bench_json_simd_std26_sse2 PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
        target_compile_features(conflux_bench_json_simd_std26_sse2 PRIVATE cxx_std_26)
        target_compile_options(conflux_bench_json_simd_std26_sse2 PRIVATE ${_conflux_bench_json_std26_options_sse2})
        target_compile_definitions(conflux_bench_json_simd_std26_sse2 PRIVATE
            CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD=conflux_json_scan_str_until_special_std26_sse2
            CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD=conflux_json_scan_dump_safe_run_std26_sse2)

        add_library(conflux_bench_json_simd_std26_avx2 OBJECT ../src/json_simd_std26.cxx)
        set_target_properties(conflux_bench_json_simd_std26_avx2 PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
        target_compile_features(conflux_bench_json_simd_std26_avx2 PRIVATE cxx_std_26)
        target_compile_options(conflux_bench_json_simd_std26_avx2 PRIVATE ${_conflux_bench_json_std26_options_avx2})
        target_compile_definitions(conflux_bench_json_simd_std26_avx2 PRIVATE
            CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD=conflux_json_scan_str_until_special_std26_avx2
            CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD=conflux_json_scan_dump_safe_run_std26_avx2)

        target_compile_definitions(conflux_cpu_dispatch_impl_bench PRIVATE CONFLUX_BENCH_HAS_JSON_STD26_VARIANTS=1)
        target_sources(conflux_cpu_dispatch_impl_bench PRIVATE
            $<TARGET_OBJECTS:conflux_bench_json_simd_std26_sse2>
            $<TARGET_OBJECTS:conflux_bench_json_simd_std26_avx2>)
        unset(_conflux_bench_json_std26_options_sse2)
        unset(_conflux_bench_json_std26_options_avx2)
    endif()
    if(CONFLUX_HAS_AESNI AND TARGET conflux_crypto)
        target_compile_definitions(conflux_cpu_dispatch_impl_bench PRIVATE CONFLUX_BENCH_HAS_AESNI=1)
        target_link_libraries(conflux_cpu_dispatch_impl_bench PRIVATE conflux_crypto)
    endif()
endif()

if(TARGET conflux)
    conflux_add_recordable_benchmark(conflux_template_bench template_bench.cxx)
    target_link_libraries(conflux_template_bench PRIVATE conflux conflux_options)
endif()

if(TARGET conflux_work)
    conflux_add_recordable_benchmark(conflux_task_creation_bench task_creation_bench.cxx)
    target_link_libraries(conflux_task_creation_bench PRIVATE bench_common conflux_work conflux_options)

    conflux_add_recordable_benchmark(conflux_task_chain_composition_bench task_chain_composition_bench.cxx)
    target_link_libraries(conflux_task_chain_composition_bench PRIVATE bench_common conflux_work conflux_options)

    conflux_add_recordable_benchmark(conflux_task_cancellation_bench task_cancellation_bench.cxx)
    target_link_libraries(conflux_task_cancellation_bench PRIVATE bench_common conflux_work conflux_options)

    conflux_add_recordable_benchmark(conflux_workpool_enqueue_dequeue_bench workpool_enqueue_dequeue_bench.cxx)
    target_link_libraries(conflux_workpool_enqueue_dequeue_bench PRIVATE bench_common conflux_work conflux_options)

    conflux_add_recordable_benchmark(conflux_workpool_queue_mode_compare_bench workpool_enqueue_dequeue_bench.cxx)
    target_compile_definitions(conflux_workpool_queue_mode_compare_bench PRIVATE CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE=1)
    target_link_libraries(conflux_workpool_queue_mode_compare_bench PRIVATE bench_common conflux_work conflux_options)

    conflux_add_recordable_benchmark(conflux_join_all_N_bench join_all_N_bench.cxx)
    target_link_libraries(conflux_join_all_N_bench PRIVATE bench_common conflux_work conflux_options)
endif()

if(TARGET conflux_http1)
    conflux_add_recordable_benchmark(conflux_http_parser_bench http_parser_bench.cxx)
    target_link_libraries(conflux_http_parser_bench
        PRIVATE bench_common conflux_http1 conflux_net_config conflux_options)
endif()

if(TARGET conflux_http1 AND TARGET conflux_http_server_helpers)
    conflux_add_recordable_benchmark(conflux_http_app_path_bench http_app_path_bench.cxx)
    target_link_libraries(conflux_http_app_path_bench
        PRIVATE bench_common conflux_http1 conflux_http_parse_helpers conflux_http_router
                conflux_http_response conflux_http_server_helpers conflux_http_app conflux_json
                conflux_net_config conflux_options)
endif()

if(TARGET conflux_http1)
    conflux_add_recordable_benchmark(conflux_http_adversarial_bench http_adversarial_bench.cxx)
    target_link_libraries(conflux_http_adversarial_bench
        PRIVATE bench_common conflux_http1 conflux_http_parse_helpers conflux_http_core
                conflux_net_config conflux_options)
endif()

if(TARGET conflux AND TARGET conflux_work)
    conflux_add_recordable_benchmark(conflux_http_server_bench http_server_bench.cxx)
    target_link_libraries(conflux_http_server_bench
        PRIVATE bench_common conflux conflux_work conflux_options)
endif()

if(TARGET conflux AND TARGET conflux_work)
    conflux_add_recordable_benchmark(conflux_static_strategy_matrix_bench static_strategy_matrix_bench.cxx)
    target_link_libraries(conflux_static_strategy_matrix_bench
        PRIVATE bench_common conflux conflux_work conflux_options)
endif()

if(CONFLUX_HAS_TLS STREQUAL "true")
    if(TARGET conflux_http_server_bench)
        target_link_libraries(conflux_http_server_bench PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(conflux_http_server_bench PRIVATE CONFLUX_BENCH_HAS_TLS=1)
    endif()
    if(TARGET conflux_static_strategy_matrix_bench)
        target_link_libraries(conflux_static_strategy_matrix_bench PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(conflux_static_strategy_matrix_bench PRIVATE CONFLUX_BENCH_HAS_TLS=1)
    endif()
endif()

if(CONFLUX_EXPERIMENTAL_SEND_ZC)
    conflux_add_recordable_benchmark(conflux_send_zc_bench send_zc_bench.cxx)
    target_link_libraries(conflux_send_zc_bench
        PRIVATE bench_common conflux conflux_work conflux_options)
    if(CONFLUX_HAS_TLS STREQUAL "true")
        target_link_libraries(conflux_send_zc_bench PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(conflux_send_zc_bench PRIVATE CONFLUX_BENCH_HAS_TLS=1)
    endif()
endif()

if(TARGET conflux AND TARGET conflux_work)
    conflux_add_recordable_benchmark(conflux_http_server_concurrency_bench http_server_concurrency_bench.cxx)
    target_link_libraries(conflux_http_server_concurrency_bench
        PRIVATE bench_common conflux conflux_work conflux_options)
endif()

if(TARGET conflux AND TARGET conflux_work)
    conflux_add_recordable_benchmark(conflux_slow_consumer_backpressure_bench slow_consumer_backpressure_bench.cxx)
    target_link_libraries(conflux_slow_consumer_backpressure_bench
        PRIVATE bench_common conflux conflux_work conflux_options)
endif()

if(TARGET conflux_socket_io AND TARGET conflux_direct_slot_pool)
    conflux_add_recordable_benchmark(conflux_socket_raw_bench socket_raw_bench.cxx)
    target_link_libraries(conflux_socket_raw_bench
        PRIVATE bench_common conflux_socket_io conflux_direct_slot_pool conflux_options)
endif()

get_property(_record_targets GLOBAL PROPERTY CONFLUX_RECORD_BENCH_TARGETS)
add_custom_target(conflux_record_benches DEPENDS ${_record_targets})
