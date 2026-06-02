conflux_add_module_library(conflux_types
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/small_function.cxx
        ${CONFLUX_SRC_ROOT}/types.cppm
        ${CONFLUX_SRC_ROOT}/types_api.cxx
)
target_compile_features(conflux_types PUBLIC cxx_std_23)
target_link_libraries(conflux_types
    PRIVATE conflux_options
)

add_library(conflux_core INTERFACE)
target_compile_features(conflux_core INTERFACE cxx_std_23)
if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    target_compile_definitions(conflux_core INTERFACE CONFLUX_INTERFACE_MODULE=1)
endif()

add_library(conflux_cpu_features STATIC ${CONFLUX_SRC_ROOT}/cpu_features.cxx)
target_link_libraries(conflux_cpu_features PRIVATE conflux_options)
