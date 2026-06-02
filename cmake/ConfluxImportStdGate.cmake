function(conflux_enable_import_std_experiment)
    set(_conflux_warn_unsupported FALSE)
    foreach(_conflux_arg IN LISTS ARGN)
        if(_conflux_arg STREQUAL "WARN_UNSUPPORTED")
            set(_conflux_warn_unsupported TRUE)
        else()
            message(FATAL_ERROR "unknown conflux_enable_import_std_experiment argument: ${_conflux_arg}")
        endif()
    endforeach()

    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3.0")
        # Covers 4.3.0 up to future stable releases.
        set(_conflux_import_std_uuid "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.3")
        # Covers 4.0.3 up to 4.2.x.
        set(_conflux_import_std_uuid "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.0")
        # Covers 4.0.0, 4.0.1, and 4.0.2.
        set(_conflux_import_std_uuid "a9e1cf81-9932-4810-974b-6eccaf14e457")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31.8")
        # Covers late 3.31 patches backported with 4.1 features.
        set(_conflux_import_std_uuid "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.30.0")
        # Covers initial experimental introduction up to 3.31.7.
        set(_conflux_import_std_uuid "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
    endif()

    if(DEFINED _conflux_import_std_uuid)
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "${_conflux_import_std_uuid}" PARENT_SCOPE)
    elseif(_conflux_warn_unsupported)
        message(WARNING "Your CMake version does not support experimental 'import std'.")
    endif()
endfunction()

function(conflux_pre_project_import_std_requested out)
    if(DEFINED CONFLUX_INTERFACE_MODE)
        set(_conflux_interface_mode "${CONFLUX_INTERFACE_MODE}")
    else()
        set(_conflux_interface_mode "MODULE_INTERFACE")
    endif()
    if(DEFINED CONFLUX_USE_IMPORT_STD)
        set(_conflux_use_import_std "${CONFLUX_USE_IMPORT_STD}")
    else()
        set(_conflux_use_import_std "AUTO")
    endif()

    set(_conflux_requested FALSE)
    if(_conflux_interface_mode STREQUAL "HEADER_INTERFACE")
        if(CONFLUX_HEADER_USE_IMPORT_STD OR CONFLUX_HEADER_USE_IMPORT_STD_COMPAT)
            set(_conflux_requested TRUE)
        endif()
    elseif(NOT _conflux_use_import_std STREQUAL "OFF")
        set(_conflux_requested TRUE)
    endif()
    set(${out} "${_conflux_requested}" PARENT_SCOPE)
endfunction()

function(conflux_configure_std_module_sources)
    set(_conflux_options SUPPRESS_CLANG_RESERVED_MODULE_IDENTIFIER_WARNING)
    set(_conflux_one_value_args REFLECTION_OPTIONS)
    cmake_parse_arguments(PARSE_ARGV 0 _conflux_std_modules
        "${_conflux_options}" "${_conflux_one_value_args}" "")

    if(_conflux_std_modules_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "unknown conflux_configure_std_module_sources arguments: ${_conflux_std_modules_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT CMAKE_CXX_STDLIB_MODULES_JSON)
        return()
    endif()

    set(_conflux_apply_clang_std_module_options OFF)
    if(_conflux_std_modules_SUPPRESS_CLANG_RESERVED_MODULE_IDENTIFIER_WARNING
            AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_conflux_apply_clang_std_module_options ON)
    endif()

    if(NOT _conflux_std_modules_REFLECTION_OPTIONS
            AND NOT _conflux_apply_clang_std_module_options)
        return()
    endif()

    cmake_path(GET CMAKE_CXX_STDLIB_MODULES_JSON PARENT_PATH _conflux_std_modules_dir)
    file(READ "${CMAKE_CXX_STDLIB_MODULES_JSON}" _conflux_std_modules_json)
    string(JSON _conflux_std_modules_count LENGTH "${_conflux_std_modules_json}" modules)
    if(_conflux_std_modules_count LESS_EQUAL 0)
        return()
    endif()

    math(EXPR _conflux_std_modules_last "${_conflux_std_modules_count} - 1")
    foreach(_conflux_std_module_index RANGE 0 ${_conflux_std_modules_last})
        string(JSON _conflux_std_module_source
            GET "${_conflux_std_modules_json}" modules ${_conflux_std_module_index} source-path)
        cmake_path(ABSOLUTE_PATH _conflux_std_module_source
            BASE_DIRECTORY "${_conflux_std_modules_dir}"
            OUTPUT_VARIABLE _conflux_std_module_source_abs)
        if(_conflux_std_modules_REFLECTION_OPTIONS)
            set_source_files_properties(
                "${_conflux_std_module_source_abs}"
                PROPERTIES COMPILE_OPTIONS "${_conflux_std_modules_REFLECTION_OPTIONS}")
        endif()
        if(_conflux_apply_clang_std_module_options)
            set_property(
                SOURCE "${_conflux_std_module_source_abs}"
                APPEND PROPERTY COMPILE_OPTIONS -Wno-reserved-module-identifier)
        endif()
    endforeach()
endfunction()
