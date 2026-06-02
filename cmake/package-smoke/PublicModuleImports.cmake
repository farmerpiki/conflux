function(conflux_package_append_target_closure out target)
    if(NOT TARGET ${target})
        return()
    endif()
    set(_seen ${${out}})
    if("${target}" IN_LIST _seen)
        return()
    endif()
    list(APPEND _seen ${target})
    foreach(_prop IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_libs ${target} ${_prop})
        if(NOT _libs)
            continue()
        endif()
        foreach(_lib IN LISTS _libs)
            if(_lib MATCHES "conflux::[A-Za-z0-9_]+")
                string(REGEX MATCH "conflux::[A-Za-z0-9_]+" _dep "${_lib}")
                if(TARGET ${_dep})
                    set(${out} "${_seen}" PARENT_SCOPE)
                    conflux_package_append_target_closure(${out} ${_dep})
                    set(_seen ${${out}})
                endif()
            endif()
        endforeach()
    endforeach()
    set(${out} "${_seen}" PARENT_SCOPE)
endfunction()

function(conflux_package_collect_module_smoke_sources out)
    set(_sources)
    foreach(_target IN LISTS ARGN)
        if(NOT TARGET ${_target})
            continue()
        endif()
        get_target_property(_sets ${_target} CXX_MODULE_SETS)
        if(_sets)
            foreach(_set IN LISTS _sets)
                get_target_property(_set_sources ${_target} CXX_MODULE_SET_${_set})
                if(_set_sources)
                    list(APPEND _sources ${_set_sources})
                endif()
            endforeach()
        endif()
        get_target_property(_interface_sets ${_target} INTERFACE_CXX_MODULE_SETS)
        if(_interface_sets)
            foreach(_set IN LISTS _interface_sets)
                get_target_property(_set_sources ${_target} INTERFACE_CXX_MODULE_SET_${_set})
                if(_set_sources)
                    list(APPEND _sources ${_set_sources})
                endif()
            endforeach()
        endif()
    endforeach()
    if(_sources)
        list(REMOVE_DUPLICATES _sources)
    endif()
    set(${out} "${_sources}" PARENT_SCOPE)
endfunction()

if(CONFLUX_PACKAGE_SMOKE_PUBLIC_MODULE_IMPORTS
        AND NOT CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    message(FATAL_ERROR
        "CONFLUX_PACKAGE_SMOKE_PUBLIC_MODULE_IMPORTS requires a MODULE_INTERFACE install tree")
endif()

if(CONFLUX_PACKAGE_SMOKE_PUBLIC_MODULE_IMPORTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_conflux_module_smoke_targets)
    foreach(_component IN LISTS _conflux_find_components)
        set(_target "conflux::${_component}")
        if(TARGET ${_target})
            conflux_package_append_target_closure(
                _conflux_module_smoke_targets ${_target})
        endif()
    endforeach()
    if(_conflux_module_smoke_targets)
        list(REMOVE_DUPLICATES _conflux_module_smoke_targets)
    endif()
    conflux_package_collect_module_smoke_sources(
        _conflux_module_smoke_sources ${_conflux_module_smoke_targets})
    if(NOT _conflux_module_smoke_sources)
        message(FATAL_ERROR
            "package smoke public module import matrix found no module sources")
    endif()

    set(_conflux_module_smoke_dir
        "${CMAKE_CURRENT_BINARY_DIR}/public-module-import-smoke")
    set(_conflux_module_smoke_source_list
        "${_conflux_module_smoke_dir}/public-module-sources.txt")
    set(_conflux_module_smoke_fragment
        "${_conflux_module_smoke_dir}/public-module-imports.cmake")
    file(MAKE_DIRECTORY "${_conflux_module_smoke_dir}")
    file(WRITE "${_conflux_module_smoke_source_list}" "")
    foreach(_source IN LISTS _conflux_module_smoke_sources)
        file(APPEND "${_conflux_module_smoke_source_list}" "${_source}\n")
    endforeach()

    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/../../scripts/generate-public-module-import-smoke.py"
                --source-list "${_conflux_module_smoke_source_list}"
                --out-dir "${_conflux_module_smoke_dir}/sources"
                --cmake-fragment "${_conflux_module_smoke_fragment}"
        RESULT_VARIABLE _conflux_module_smoke_result)
    if(NOT _conflux_module_smoke_result EQUAL 0)
        message(FATAL_ERROR
            "package smoke public module import matrix generation failed")
    endif()
    include("${_conflux_module_smoke_fragment}")
    if(NOT CONFLUX_PUBLIC_MODULE_SMOKE_SOURCES)
        message(FATAL_ERROR
            "package smoke public module import matrix generated no sources")
    endif()

    add_library(conflux_package_smoke_public_module_imports OBJECT
        ${CONFLUX_PUBLIC_MODULE_SMOKE_SOURCES})
    target_compile_features(conflux_package_smoke_public_module_imports PRIVATE cxx_std_23)
    conflux_apply_package_smoke_build_policy(
        conflux_package_smoke_public_module_imports)
    target_link_libraries(conflux_package_smoke_public_module_imports PRIVATE
        ${_conflux_module_smoke_targets})
endif()
