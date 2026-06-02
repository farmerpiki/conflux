add_library(conflux_options INTERFACE)
conflux_apply_compiler_options(conflux_options)
target_include_directories(conflux_options INTERFACE
    "$<BUILD_INTERFACE:${CONFLUX_SRC_ROOT}>"
    "$<BUILD_INTERFACE:${CONFLUX_GENERATED_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:include>"
    "$<INSTALL_INTERFACE:include/conflux/modules>"
)
target_compile_definitions(conflux_options INTERFACE
    CONFLUX_HEADER_USE_IMPORT_STD=$<BOOL:${CONFLUX_HEADER_USE_IMPORT_STD}>
    CONFLUX_HEADER_USE_IMPORT_STD_COMPAT=$<BOOL:${CONFLUX_HEADER_USE_IMPORT_STD_COMPAT}>
    CONFLUX_HEADER_USE_MODULE_IMPORTS=$<BOOL:${CONFLUX_HEADER_USE_MODULE_IMPORTS}>
    CONFLUX_MODULE_USE_IMPORT_STD=$<BOOL:${CONFLUX_IMPORT_STD_ENABLED}>
    CONFLUX_MODULE_USE_IMPORT_STD_COMPAT=0
    CONFLUX_MODULE_USE_IMPORTS=0
    CONFLUX_MODULE_REEXPORT_IMPORTS=0
    CONFLUX_ENABLE_EXPERIMENTAL=$<BOOL:${CONFLUX_ENABLE_EXPERIMENTAL}>
    CONFLUX_CPU_FEATURE_PROBES_RUNTIME=$<BOOL:${_conflux_cpu_feature_probes_runtime}>
    CONFLUX_SIMD_SELECTION_DIRECT=$<BOOL:${_conflux_simd_selection_direct}>
    CONFLUX_SIMD_SELECTION_RUNTIME=$<BOOL:${_conflux_simd_selection_runtime}>
    CONFLUX_ENABLE_RECV_BUNDLE=$<BOOL:${CONFLUX_ENABLE_RECV_BUNDLE}>
    CONFLUX_ENABLE_RECV_INCREMENTAL_BUF=$<BOOL:${CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF}>
    CONFLUX_ENABLE_SEND_ZC=$<BOOL:${CONFLUX_EXPERIMENTAL_SEND_ZC}>
    CONFLUX_ENABLE_RING_GROWTH=$<BOOL:${CONFLUX_EXPERIMENTAL_RING_GROWTH}>
    CONFLUX_ENABLE_IOPOLL_STORAGE_TEST=$<BOOL:${CONFLUX_EXPERIMENTAL_IOPOLL_STORAGE_TEST}>
    CONFLUX_HAS_WARNING_CLEAN_AUTO_UNDERSCORE_DISCARD=$<BOOL:${CONFLUX_HAS_WARNING_CLEAN_AUTO_UNDERSCORE_DISCARD}>
    CONFLUX_HAS_JSON_REFLECT=$<TARGET_EXISTS:conflux_json_reflect>
    CONFLUX_BUILD_VERSION="${PROJECT_VERSION}"
    CONFLUX_BUILD_GIT_COMMIT="${CONFLUX_GIT_COMMIT}"
    CONFLUX_BUILD_COMPILER="${CMAKE_CXX_COMPILER_ID}"
    CONFLUX_BUILD_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    CONFLUX_BUILD_STDLIB="${CONFLUX_STDLIB_NAME}"
    CONFLUX_BUILD_STDLIB_VERSION="${CONFLUX_STDLIB_VERSION}"
    CONFLUX_BUILD_TYPE_VALUE="${CMAKE_BUILD_TYPE}"
    CONFLUX_BUILD_INTERFACE_MODE="${CONFLUX_INTERFACE_MODE}"
    CONFLUX_BUILD_FEATURE_SET="${CONFLUX_FEATURE_SET}"
    CONFLUX_BUILD_API_SURFACE="${CONFLUX_API_SURFACE}"
    CONFLUX_BUILD_SIMD_SELECTION="${_conflux_simd_selection}"
    CONFLUX_BUILD_CPU_FEATURE_PROBES_RUNTIME=$<BOOL:${_conflux_cpu_feature_probes_runtime}>
)
conflux_apply_api_surface_definitions(conflux_options INTERFACE)
if(CONFLUX_ENABLE_EXPERIMENTAL)
    message(STATUS "conflux: experimental features enabled")
else()
    message(STATUS "conflux: experimental features disabled")
endif()
message(STATUS "conflux: SIMD selection resolved to ${_conflux_simd_selection}")
if(_conflux_cpu_feature_probes_runtime)
    message(STATUS "conflux: CPU feature probes use runtime detection")
else()
    message(STATUS "conflux: CPU feature probes assume selected compiled ISA is available")
endif()
if(CONFLUX_ENABLE_RECV_BUNDLE)
    message(STATUS "conflux: recv bundle enabled")
else()
    message(STATUS "conflux: recv bundle disabled")
endif()
if(CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF)
    message(STATUS "conflux: experimental incremental buf ring enabled")
else()
    message(STATUS "conflux: experimental incremental buf ring disabled")
endif()
if(CONFLUX_EXPERIMENTAL_SEND_ZC)
    message(STATUS "conflux: experimental SEND_ZC enabled")
else()
    message(STATUS "conflux: experimental SEND_ZC disabled")
endif()
if(CONFLUX_EXPERIMENTAL_RING_GROWTH)
    message(STATUS "conflux: experimental ring growth enabled")
else()
    message(STATUS "conflux: experimental ring growth disabled")
endif()
if(CONFLUX_EXPERIMENTAL_IOPOLL_STORAGE_TEST)
    message(STATUS "conflux: experimental IOPOLL storage test enabled")
else()
    message(STATUS "conflux: experimental IOPOLL storage test disabled")
endif()
if(CONFLUX_ENABLE_HTTP_TRACE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(conflux_options INTERFACE CONFLUX_HTTP_TRACE=1)
    message(STATUS "conflux: HTTP trace instrumentation enabled for Debug")
elseif(CONFLUX_ENABLE_HTTP_TRACE)
    message(STATUS "conflux: HTTP trace requested but ignored outside Debug builds")
endif()
