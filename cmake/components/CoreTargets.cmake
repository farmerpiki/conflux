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

add_library(conflux_utils STATIC)
target_sources(conflux_utils
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/utils.cxx
        ${CONFLUX_SRC_ROOT}/facade/conflux_core.cxx
)
target_link_libraries(conflux_utils
    PRIVATE conflux_options
    PRIVATE conflux_cpu_features
    PUBLIC  conflux_types
)
if(NOT _conflux_simd_backend STREQUAL "OFF")
    target_compile_definitions(conflux_utils PRIVATE CONFLUX_STDSIMD=1)
    target_link_libraries(conflux_utils PRIVATE conflux_simd_runtime)
endif()

# Keep the package-facing core anchor aligned with the documented
# "types + utils" core surface while leaving HTTP configuration as a separate
# support component.
target_link_libraries(conflux_core INTERFACE conflux_utils)
