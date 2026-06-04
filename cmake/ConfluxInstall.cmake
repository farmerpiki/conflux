# ---------------------------------------------------------------------------
# Public package targets / install-export
# ---------------------------------------------------------------------------

include(ConfluxHeaderInstall)

set(CONFLUX_PACKAGE_COMPONENTS)
set(CONFLUX_PACKAGE_TARGETS)
set(CONFLUX_PACKAGE_EXPLICIT_COMPONENTS)
set(CONFLUX_PACKAGE_EXPLICIT_TARGETS)
set(CONFLUX_PACKAGE_EXPERIMENTAL_COMPONENTS)
set(CONFLUX_PACKAGE_EXPERIMENTAL_TARGETS)
set(CONFLUX_PACKAGE_SUPPORT_COMPONENTS)
set(CONFLUX_PACKAGE_SUPPORT_TARGETS)
set(CONFLUX_PACKAGE_ALL_COMPONENTS)
set(CONFLUX_PACKAGE_ALL_TARGETS)
set(CONFLUX_PUBLIC_COMPONENT_TARGETS)
set(CONFLUX_INSTALL_TARGET_CANDIDATES)

include(ConfluxComponentRegistry)

function(conflux_requestable_component_enabled out export_name)
    set(_enabled TRUE)
    if(export_name STREQUAL "http_compression")
        set(_enabled "${CONFLUX_WANT_HTTP_COMPRESSION}")
    elseif(export_name STREQUAL "http_observability")
        set(_enabled "${CONFLUX_WANT_HTTP_OBSERVABILITY}")
    elseif(export_name STREQUAL "http_openapi")
        set(_enabled "${CONFLUX_WANT_HTTP_OPENAPI}")
    elseif(export_name STREQUAL "http_proxy")
        set(_enabled "${CONFLUX_WANT_HTTP_PROXY}")
    elseif(export_name STREQUAL "http_static")
        set(_enabled "${CONFLUX_WANT_HTTP_STATIC}")
    elseif(export_name STREQUAL "http_realtime")
        set(_enabled "${CONFLUX_WANT_HTTP_REALTIME}")
    elseif(export_name STREQUAL "http_vhost")
        set(_enabled "${CONFLUX_WANT_HTTP_VHOST}")
    endif()
    set(${out} "${_enabled}" PARENT_SCOPE)
endfunction()

function(conflux_component target export_name)
    set(options)
    set(one_value_args KIND)
    set(multi_value_args)
    cmake_parse_arguments(CONFLUX_COMPONENT
        "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(NOT CONFLUX_COMPONENT_KIND)
        set(CONFLUX_COMPONENT_KIND REQUESTABLE)
    endif()
    if(NOT CONFLUX_COMPONENT_KIND MATCHES "^(REQUESTABLE|EXPLICIT|EXPERIMENTAL|SUPPORT)$")
        message(FATAL_ERROR
            "conflux: component '${export_name}' has invalid KIND '${CONFLUX_COMPONENT_KIND}'")
    endif()

    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
        if(NOT TARGET conflux::${export_name})
            add_library(conflux::${export_name} ALIAS ${target})
        endif()

        set(_all_components ${CONFLUX_PACKAGE_ALL_COMPONENTS})
        list(APPEND _all_components "${export_name}")
        set(CONFLUX_PACKAGE_ALL_COMPONENTS "${_all_components}" PARENT_SCOPE)

        set(_all_targets ${CONFLUX_PACKAGE_ALL_TARGETS})
        list(APPEND _all_targets "conflux::${export_name}")
        set(CONFLUX_PACKAGE_ALL_TARGETS "${_all_targets}" PARENT_SCOPE)

        set(_install_target_candidates ${CONFLUX_INSTALL_TARGET_CANDIDATES})
        list(APPEND _install_target_candidates ${target})
        set(CONFLUX_INSTALL_TARGET_CANDIDATES
            "${_install_target_candidates}" PARENT_SCOPE)

        conflux_requestable_component_enabled(_requestable_component_enabled "${export_name}")

        if(CONFLUX_COMPONENT_KIND STREQUAL "SUPPORT"
                OR (CONFLUX_COMPONENT_KIND STREQUAL "REQUESTABLE"
                    AND NOT _requestable_component_enabled))
            set(_support_components ${CONFLUX_PACKAGE_SUPPORT_COMPONENTS})
            list(APPEND _support_components "${export_name}")
            set(CONFLUX_PACKAGE_SUPPORT_COMPONENTS "${_support_components}" PARENT_SCOPE)

            set(_support_targets ${CONFLUX_PACKAGE_SUPPORT_TARGETS})
            list(APPEND _support_targets "conflux::${export_name}")
            set(CONFLUX_PACKAGE_SUPPORT_TARGETS "${_support_targets}" PARENT_SCOPE)
        elseif(CONFLUX_COMPONENT_KIND STREQUAL "EXPLICIT")
            set(_explicit_components ${CONFLUX_PACKAGE_EXPLICIT_COMPONENTS})
            list(APPEND _explicit_components "${export_name}")
            set(CONFLUX_PACKAGE_EXPLICIT_COMPONENTS "${_explicit_components}" PARENT_SCOPE)

            set(_explicit_targets ${CONFLUX_PACKAGE_EXPLICIT_TARGETS})
            list(APPEND _explicit_targets "conflux::${export_name}")
            set(CONFLUX_PACKAGE_EXPLICIT_TARGETS "${_explicit_targets}" PARENT_SCOPE)
        elseif(CONFLUX_COMPONENT_KIND STREQUAL "EXPERIMENTAL")
            set(_experimental_components ${CONFLUX_PACKAGE_EXPERIMENTAL_COMPONENTS})
            list(APPEND _experimental_components "${export_name}")
            set(CONFLUX_PACKAGE_EXPERIMENTAL_COMPONENTS "${_experimental_components}" PARENT_SCOPE)

            set(_experimental_targets ${CONFLUX_PACKAGE_EXPERIMENTAL_TARGETS})
            list(APPEND _experimental_targets "conflux::${export_name}")
            set(CONFLUX_PACKAGE_EXPERIMENTAL_TARGETS "${_experimental_targets}" PARENT_SCOPE)
        elseif(_requestable_component_enabled)
            set(_components ${CONFLUX_PACKAGE_COMPONENTS})
            list(APPEND _components "${export_name}")
            set(CONFLUX_PACKAGE_COMPONENTS "${_components}" PARENT_SCOPE)

            set(_targets ${CONFLUX_PACKAGE_TARGETS})
            list(APPEND _targets "conflux::${export_name}")
            set(CONFLUX_PACKAGE_TARGETS "${_targets}" PARENT_SCOPE)

            set(_public_targets ${CONFLUX_PUBLIC_COMPONENT_TARGETS})
            list(APPEND _public_targets ${target})
            set(CONFLUX_PUBLIC_COMPONENT_TARGETS
                "${_public_targets}" PARENT_SCOPE)
        endif()

    endif()
endfunction()

macro(conflux_public_component target export_name)
    conflux_component(${target} ${export_name} KIND REQUESTABLE)
endmacro()

macro(conflux_explicit_component target export_name)
    conflux_component(${target} ${export_name} KIND EXPLICIT)
endmacro()

macro(conflux_experimental_component target export_name)
    conflux_component(${target} ${export_name} KIND EXPERIMENTAL)
endmacro()

function(conflux_target_has_module_file_sets out target)
    set(_has_module_file_sets FALSE)
    foreach(_property IN ITEMS CXX_MODULE_SETS INTERFACE_CXX_MODULE_SETS)
        get_target_property(_sets ${target} ${_property})
        if(_sets)
            set(_has_module_file_sets TRUE)
            break()
        endif()
    endforeach()
    set(${out} ${_has_module_file_sets} PARENT_SCOPE)
endfunction()

foreach(_entry IN LISTS CONFLUX_PUBLIC_COMPONENT_DECLARATIONS)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _target)
    list(GET _parts 1 _export_name)
    conflux_public_component(${_target} ${_export_name})
endforeach()

foreach(_entry IN LISTS CONFLUX_EXPLICIT_COMPONENT_DECLARATIONS)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _target)
    list(GET _parts 1 _export_name)
    conflux_explicit_component(${_target} ${_export_name})
endforeach()

foreach(_entry IN LISTS CONFLUX_EXPERIMENTAL_COMPONENT_DECLARATIONS)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _target)
    list(GET _parts 1 _export_name)
    conflux_experimental_component(${_target} ${_export_name})
endforeach()

# Internal-but-exported support targets needed by static-library link interfaces.
foreach(_entry IN LISTS CONFLUX_SUPPORT_COMPONENT_DECLARATIONS)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _target)
    list(GET _parts 1 _export_name)
    conflux_component(${_target} ${_export_name} KIND SUPPORT)
endforeach()

if(TARGET conflux AND NOT TARGET conflux::conflux)
    add_library(conflux::conflux ALIAS conflux)
endif()

set(CONFLUX_INSTALL_INTERFACE_TARGETS)
set(CONFLUX_INSTALL_MODULE_TARGETS)
list(REMOVE_DUPLICATES CONFLUX_INSTALL_TARGET_CANDIDATES)
foreach(_target IN LISTS CONFLUX_INSTALL_TARGET_CANDIDATES)
    if(TARGET ${_target})
        if(_target STREQUAL "conflux")
            continue()
        endif()
        conflux_target_has_module_file_sets(_has_module_file_sets ${_target})
        if(_has_module_file_sets)
            list(APPEND CONFLUX_INSTALL_MODULE_TARGETS ${_target})
        else()
            list(APPEND CONFLUX_INSTALL_INTERFACE_TARGETS ${_target})
        endif()
    endif()
endforeach()

foreach(_target IN LISTS CONFLUX_INSTALL_MODULE_TARGETS)
    if(NOT _target STREQUAL "conflux_types")
        get_target_property(_target_type ${_target} TYPE)
        if(_target_type STREQUAL "INTERFACE_LIBRARY")
            target_link_libraries(${_target} INTERFACE conflux_types)
        else()
            target_link_libraries(${_target} PUBLIC conflux_types)
        endif()
    endif()
endforeach()

if(CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
    set(_conflux_public_module_import_smoke_targets)
    list(REMOVE_DUPLICATES CONFLUX_PUBLIC_COMPONENT_TARGETS)
    foreach(_target IN LISTS CONFLUX_PUBLIC_COMPONENT_TARGETS)
        if(TARGET ${_target})
            conflux_target_has_module_file_sets(_has_module_file_sets ${_target})
            if(_has_module_file_sets)
                list(APPEND _conflux_public_module_import_smoke_targets ${_target})
            endif()
        endif()
    endforeach()
    conflux_add_public_module_import_smoke_target(
        TARGETS ${_conflux_public_module_import_smoke_targets})
    unset(_conflux_public_module_import_smoke_targets)
endif()

if(CONFLUX_INSTALL_INTERFACE_TARGETS)
    foreach(_target IN LISTS CONFLUX_INSTALL_INTERFACE_TARGETS)
        get_target_property(_export_name ${_target} EXPORT_NAME)
        install(TARGETS ${_target}
            EXPORT confluxTargets-${_export_name}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endforeach()
endif()

if(CONFLUX_INSTALL_MODULE_TARGETS)
    foreach(_target IN LISTS CONFLUX_INSTALL_MODULE_TARGETS)
        get_target_property(_export_name ${_target} EXPORT_NAME)
        install(TARGETS ${_target}
            EXPORT confluxTargets-${_export_name}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules
            FILE_SET generated_mods DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/generated/modules
        )
    endforeach()
endif()

if(TARGET conflux)
    install(TARGETS conflux
        EXPORT confluxTargets-umbrella
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules
        FILE_SET generated_mods DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/generated/modules
    )
endif()

set(_conflux_install_targets_for_runtime_scan
    ${CONFLUX_INSTALL_INTERFACE_TARGETS}
    ${CONFLUX_INSTALL_MODULE_TARGETS})
if(TARGET conflux)
    list(APPEND _conflux_install_targets_for_runtime_scan conflux)
endif()
conflux_any_target_links_item(CONFLUX_INSTALL_NEEDS_LIBURING
    PkgConfig::LIBURING
    ${_conflux_install_targets_for_runtime_scan})
unset(_conflux_install_targets_for_runtime_scan)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConfluxGeneratePackageMetadata.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ConfluxGeneratePackageMetadata.cmake"
    @ONLY
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/conflux-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConfluxComponentRegistry.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConfluxExternalDependencyRegistry.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
)

set(CONFLUX_PUBLIC_HPP_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/public-hpp/conflux")
conflux_register_header_public_hpp(config)
conflux_register_header_public_profile_hpps()
foreach(_component IN LISTS CONFLUX_PACKAGE_COMPONENTS)
    if(EXISTS "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/${_component}.hxx")
        conflux_register_header_public_hpp("${_component}")
    endif()
endforeach()
foreach(_component IN ITEMS http json work)
    if(_component IN_LIST CONFLUX_PACKAGE_COMPONENTS)
        set_property(GLOBAL APPEND PROPERTY
            CONFLUX_HEADER_PUBLIC_HPP_TOP_LEVEL_NAMES "${_component}")
    endif()
endforeach()
set(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS FALSE)
foreach(_component IN ITEMS
        crypto dns dns_bridge file_io file_map http http_app http_client
        http_core http_realtime http_server socket_io uring work)
    if(_component IN_LIST CONFLUX_PACKAGE_COMPONENTS)
        set(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS TRUE)
    endif()
endforeach()
set(CONFLUX_HEADER_INSTALL_DB_COMPONENTS FALSE)
if(pg IN_LIST CONFLUX_PACKAGE_COMPONENTS)
    set(CONFLUX_HEADER_INSTALL_DB_COMPONENTS TRUE)
endif()
conflux_write_header_public_hpp_files("${CONFLUX_PUBLIC_HPP_DIR}")
conflux_install_registered_public_headers()

install(FILES
    ${CONFLUX_SRC_ROOT}/cpu_features.hxx
    ${CONFLUX_SRC_ROOT}/json_simd_backend.hxx
    ${CONFLUX_SRC_ROOT}/simd_backend.hxx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules/src
)
install(FILES
    ${CONFLUX_SRC_ROOT}/cpu_features.hxx
    ${CONFLUX_SRC_ROOT}/json_simd_backend.hxx
    ${CONFLUX_SRC_ROOT}/simd_backend.hxx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules
)
install(FILES
    ${CONFLUX_SRC_ROOT}/conflux/detail/discard.hxx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/detail
)
foreach(_target IN LISTS CONFLUX_INSTALL_INTERFACE_TARGETS CONFLUX_INSTALL_MODULE_TARGETS)
    get_target_property(_export_name ${_target} EXPORT_NAME)
    install(EXPORT confluxTargets-${_export_name}
        NAMESPACE conflux::
        FILE confluxTargets-${_export_name}.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
    )
endforeach()
if(TARGET conflux)
    install(EXPORT confluxTargets-umbrella
        NAMESPACE conflux::
        FILE confluxTargets-umbrella.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
    )
endif()
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/ConfluxGeneratePackageMetadata.cmake")
