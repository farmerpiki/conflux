if(CONFLUX_HAS_DB STREQUAL "true" AND CONFLUX_WANT_DB_POSTGRES)
    add_library(conflux_pg STATIC)
    target_sources(conflux_pg
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CONFLUX_SRC_ROOT}"
            FILES
            ${CONFLUX_SRC_ROOT}/db/types.cxx
            ${CONFLUX_SRC_ROOT}/db/params.cxx
            ${CONFLUX_SRC_ROOT}/db/result.cxx
            ${CONFLUX_SRC_ROOT}/db/connection.cxx
            ${CONFLUX_SRC_ROOT}/db/pool.cxx
            ${CONFLUX_SRC_ROOT}/db/pg.cxx
    )
    target_sources(conflux_pg
        PRIVATE
            ${CONFLUX_SRC_ROOT}/db/result_impl.cxx
    )
    target_link_libraries(conflux_pg
        PRIVATE conflux_options
        PUBLIC  conflux_work
        PUBLIC  conflux_uring
        PUBLIC  conflux_uring_timeout
        PUBLIC  conflux_file_io_sync
        PUBLIC  conflux_file_io
        PUBLIC  PkgConfig::LIBPQ
    )
    target_compile_definitions(conflux_pg PUBLIC CONFLUX_HAS_DB=1)

endif()
