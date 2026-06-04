if(CONFLUX_NEEDS_RUNTIME)

conflux_add_module_library(conflux_uring_primitives
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/uring/uring_fd.cxx
        ${CONFLUX_SRC_ROOT}/uring/uring_sqe.cxx
)
target_link_libraries(conflux_uring_primitives
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  PkgConfig::LIBURING
)

conflux_add_module_library(conflux_uring
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/uring/uring.cxx
        ${CONFLUX_SRC_ROOT}/uring/flow.cxx
        ${CONFLUX_SRC_ROOT}/uring/uring_completion.cxx
        ${CONFLUX_SRC_ROOT}/uring/uring_handle.cxx
)
target_link_libraries(conflux_uring
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_uring_primitives
    PUBLIC  PkgConfig::LIBURING
)

conflux_add_module_library(conflux_uring_timeout
    PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/uring/uring_timeout.cxx
)
target_link_libraries(conflux_uring_timeout
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
    PUBLIC  conflux_uring
    PUBLIC  PkgConfig::LIBURING
)

conflux_configure_uring_probes(conflux_uring)

conflux_add_module_library(conflux_work
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/work/carrier_flags.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_model_a.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_scope.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_deadline.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_coro.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_streams.cxx
        ${CONFLUX_SRC_ROOT}/work/carrier_timer.cxx
        ${CONFLUX_SRC_ROOT}/work/race.cxx
        ${CONFLUX_SRC_ROOT}/work/root_core.cxx
        ${CONFLUX_SRC_ROOT}/work/root_tasks.cxx
        ${CONFLUX_SRC_ROOT}/work/root.cxx
        ${CONFLUX_SRC_ROOT}/work.cppm
        ${CONFLUX_SRC_ROOT}/work_api.cxx
    PRIVATE_SOURCES
        ${CONFLUX_SRC_ROOT}/work_impl.cxx
)
target_compile_definitions(conflux_work
    PUBLIC CONFLUX_WORK_CARRIER_MODEL_A=$<BOOL:${CONFLUX_WORK_CARRIER_MODEL_A}>
    PUBLIC CONFLUX_WORK_CARRIER_MODEL_B=$<BOOL:${CONFLUX_WORK_CARRIER_MODEL_B}>
    PUBLIC CONFLUX_WORK_CORO_FRAME_POOL=$<BOOL:${CONFLUX_WORK_CORO_FRAME_POOL}>
    PUBLIC CONFLUX_WORK_ALLOC_STATS=$<BOOL:${CONFLUX_WORK_ALLOC_STATS}>
    PUBLIC CONFLUX_WORK_QUEUE_STATS=$<BOOL:${CONFLUX_WORK_QUEUE_STATS}>
)
target_link_libraries(conflux_work
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_uring_primitives
    PUBLIC  PkgConfig::LIBURING
)

endif() # CONFLUX_NEEDS_RUNTIME (uring + work)

if(CONFLUX_WANT_PROCESS)
conflux_add_module_library(conflux_process
    PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/process.cxx
)
target_link_libraries(conflux_process
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
)
endif() # CONFLUX_WANT_PROCESS

if(CONFLUX_NEEDS_RUNTIME)
conflux_add_module_library(conflux_net_io_buffer
    PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/net/io_buffer.cxx
)
target_link_libraries(conflux_net_io_buffer
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
)
endif() # CONFLUX_NEEDS_RUNTIME (net io buffer)

if(CONFLUX_WANT_FILE_IO)
conflux_add_module_library(conflux_file_io
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/file_io/buffers.cxx
        ${CONFLUX_SRC_ROOT}/file_io/pipe_pool.cxx
        ${CONFLUX_SRC_ROOT}/file_io/reader.cxx
        ${CONFLUX_SRC_ROOT}/file_io/iopoll.cxx
        ${CONFLUX_SRC_ROOT}/file_io/driver.cxx
        ${CONFLUX_SRC_ROOT}/file_io/file_io.cxx
)
target_link_libraries(conflux_file_io
    PRIVATE conflux_options
    PUBLIC  conflux_file_io_sync
    PUBLIC  conflux_file_map
    PUBLIC  conflux_uring
    PUBLIC  conflux_uring_timeout
    PUBLIC  conflux_work
)

endif() # CONFLUX_WANT_FILE_IO

if(CONFLUX_WANT_FILE_WATCH)
conflux_add_module_library(conflux_file_watch
    PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/file_watch.cxx
)
target_link_libraries(conflux_file_watch
    PRIVATE conflux_options
    PUBLIC  conflux_file_io
)
endif() # CONFLUX_WANT_FILE_WATCH

if(CONFLUX_WANT_SOCKET_IO)
conflux_add_module_library(conflux_socket_io
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/socket_io/socket_io.cxx
        ${CONFLUX_SRC_ROOT}/socket_io/socket_io_coro.cxx
        ${CONFLUX_SRC_ROOT}/socket_io/socket_io_blocking.cxx
    PRIVATE_SOURCES
        ${CONFLUX_SRC_ROOT}/socket_io/direct_fd_table.cxx
        ${CONFLUX_SRC_ROOT}/socket_io/tcp_listener.cxx
        ${CONFLUX_SRC_ROOT}/socket_io/socket_io_coro_impl.cxx
)

target_link_libraries(conflux_socket_io
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_uring
    PUBLIC  conflux_work
)

endif() # CONFLUX_WANT_SOCKET_IO

if(CONFLUX_NEEDS_RUNTIME)
conflux_add_module_library(conflux_net_cancel
    PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/net/cancel.cxx
    PRIVATE_SOURCES
        ${CONFLUX_SRC_ROOT}/net/cancel_impl.cxx
)
target_link_libraries(conflux_net_cancel
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
)
endif() # CONFLUX_NEEDS_RUNTIME (net cancel)
