function(conflux_add_module_library target)
    set(options)
    set(one_value_args)
    set(multi_value_args PUBLIC_MODULES PRIVATE_SOURCES)
    cmake_parse_arguments(CONFLUX_MODULE_LIBRARY
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN})
    if(CONFLUX_MODULE_LIBRARY_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "conflux_add_module_library(${target}) got unexpected arguments: "
            "${CONFLUX_MODULE_LIBRARY_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT CONFLUX_MODULE_LIBRARY_PUBLIC_MODULES)
        message(FATAL_ERROR
            "conflux_add_module_library(${target}) requires PUBLIC_MODULES")
    endif()
    add_library(${target} STATIC)
    target_sources(${target}
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CONFLUX_SRC_ROOT}"
            FILES ${CONFLUX_MODULE_LIBRARY_PUBLIC_MODULES}
    )
    if(CONFLUX_MODULE_LIBRARY_PRIVATE_SOURCES)
        target_sources(${target}
            PRIVATE ${CONFLUX_MODULE_LIBRARY_PRIVATE_SOURCES}
        )
    endif()
endfunction()
