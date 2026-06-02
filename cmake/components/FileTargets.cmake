if(CONFLUX_EFFECTIVE_FILE_IO_SYNC)
conflux_add_module_library(conflux_file_io_sync
    PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/file_io/file_io_sync.cxx
)
target_compile_features(conflux_file_io_sync PUBLIC cxx_std_23)
target_link_libraries(conflux_file_io_sync
    PRIVATE conflux_options
    PUBLIC  conflux_types
)
endif() # CONFLUX_EFFECTIVE_FILE_IO_SYNC

if(CONFLUX_WANT_FILE_MAP)
conflux_add_module_library(conflux_file_map
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/file_io/file_map_types.cxx
        ${CONFLUX_SRC_ROOT}/file_io/file_map.cxx
)
target_link_libraries(conflux_file_map
    PRIVATE conflux_options
    PUBLIC  conflux_file_io_sync
)
endif() # CONFLUX_WANT_FILE_MAP

if(CONFLUX_WANT_JSON_FILE)
conflux_add_module_library(conflux_json_file
    PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/json_file.cxx
)
target_compile_features(conflux_json_file PUBLIC cxx_std_23)
target_link_libraries(conflux_json_file
    PRIVATE conflux_options
    PUBLIC  conflux_json conflux_file_io_sync
)
endif() # CONFLUX_WANT_JSON_FILE
