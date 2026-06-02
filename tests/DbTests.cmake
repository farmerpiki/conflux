if(CONFLUX_HAS_DB STREQUAL "true")
    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_legacy_module
        SOURCE db_compile_fail_legacy_module.cxx
        TEST api-surface/db-hides-legacy-module
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux.db")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_pg_error
        SOURCE db_compile_fail_global_pg_error.cxx
        TEST api-surface/db-hides-global-pg-error
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "PgError")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_column
        SOURCE db_compile_fail_global_column.cxx
        TEST api-surface/db-hides-global-column
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Column")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_pool_config
        SOURCE db_compile_fail_global_pool_config.cxx
        TEST api-surface/db-hides-global-pool-config
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "PoolConfig")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_tx_options
        SOURCE db_compile_fail_global_tx_options.cxx
        TEST api-surface/db-hides-global-tx-options
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "TxOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_connect_params
        SOURCE db_compile_fail_global_connect_params.cxx
        TEST api-surface/db-hides-global-connect-params
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "ConnectParams")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_query_options
        SOURCE db_compile_fail_global_query_options.cxx
        TEST api-surface/db-hides-global-query-options
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "QueryOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_statement_cache
        SOURCE db_compile_fail_global_statement_cache.cxx
        TEST api-surface/db-hides-global-statement-cache
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "StatementCache")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_connection
        SOURCE db_compile_fail_global_connection.cxx
        TEST api-surface/db-hides-global-connection
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Connection")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_pipeline
        SOURCE db_compile_fail_global_pipeline.cxx
        TEST api-surface/db-hides-global-pipeline
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Pipeline")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_pool
        SOURCE db_compile_fail_global_pool.cxx
        TEST api-surface/db-hides-global-pool
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Pool")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_query_cache
        SOURCE db_compile_fail_global_query_cache.cxx
        TEST api-surface/db-hides-global-query-cache
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "QueryCache")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_row
        SOURCE db_compile_fail_global_row.cxx
        TEST api-surface/db-hides-global-row
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Row")

    conflux_add_compile_fail_test(
        TARGET conflux_db_compile_fail_global_result
        SOURCE db_compile_fail_global_result.cxx
        TEST api-surface/db-hides-global-result
        LINK conflux_pg conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Result")

    add_executable(conflux_db_tests db_test.cxx)
    target_link_libraries(conflux_db_tests
        PRIVATE
            conflux_pg
            conflux_work
            conflux_file_io
            conflux_options
            Catch2::Catch2WithMain
            PkgConfig::LIBPQ
    )

    add_executable(conflux_db_integration db_integration_test.cxx)
    target_link_libraries(conflux_db_integration
        PRIVATE
            conflux_pg
            conflux_work
            conflux_file_io
            conflux_options
            Catch2::Catch2WithMain
            PkgConfig::LIBPQ
    )
endif()
