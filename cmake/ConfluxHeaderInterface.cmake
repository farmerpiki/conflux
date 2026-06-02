include(Dependencies)
include(ConfluxProviderResolution)
include(ConfluxComponentValidation)
include(ConfluxComponentRegistry)
include(ConfluxHeaderInstall)
if(CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
    enable_testing()
endif()
conflux_add_header_interface_target()

macro(conflux_header_public_component target export_name)
    set(options HPP_TOP_LEVEL NO_PACKAGE)
    set(one_value_args)
    set(multi_value_args COMPILE_DEFINITIONS IMPLS LINKS MODULE_PREFIXES)
    cmake_parse_arguments(CONFLUX_HEADER_PUBLIC_COMPONENT
        "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    add_library(${target} INTERFACE)
    target_link_libraries(${target} INTERFACE conflux_headers)
    if(CONFLUX_HEADER_PUBLIC_COMPONENT_IMPLS)
        conflux_link_existing_header_impls(${target}
            ${CONFLUX_HEADER_PUBLIC_COMPONENT_IMPLS})
    endif()
    if(CONFLUX_HEADER_PUBLIC_COMPONENT_LINKS)
        target_link_libraries(${target} INTERFACE
            ${CONFLUX_HEADER_PUBLIC_COMPONENT_LINKS})
    endif()
    if(CONFLUX_HEADER_PUBLIC_COMPONENT_COMPILE_DEFINITIONS)
        target_compile_definitions(${target} INTERFACE
            ${CONFLUX_HEADER_PUBLIC_COMPONENT_COMPILE_DEFINITIONS})
    endif()

    if(CONFLUX_HEADER_PUBLIC_COMPONENT_NO_PACKAGE)
        if(NOT CONFLUX_HEADER_PUBLIC_COMPONENT_MODULE_PREFIXES)
            set(CONFLUX_HEADER_PUBLIC_COMPONENT_MODULE_PREFIXES
                conflux.${export_name})
        endif()
        conflux_register_header_public_surface(${target}
            MODULE_PREFIXES ${CONFLUX_HEADER_PUBLIC_COMPONENT_MODULE_PREFIXES})
    else()
        set(_register_args)
        if(CONFLUX_HEADER_PUBLIC_COMPONENT_HPP_TOP_LEVEL)
            list(APPEND _register_args HPP_TOP_LEVEL)
        endif()
        if(CONFLUX_HEADER_PUBLIC_COMPONENT_MODULE_PREFIXES)
            list(APPEND _register_args MODULE_PREFIXES
                ${CONFLUX_HEADER_PUBLIC_COMPONENT_MODULE_PREFIXES})
        endif()
        conflux_register_header_package_component(${target} ${export_name}
            ${_register_args})
    endif()
endmacro()

macro(conflux_header_public_component_by_export export_name)
    conflux_component_target_for_export(_conflux_header_component_target "${export_name}")
    conflux_header_public_component(${_conflux_header_component_target} ${export_name} ${ARGN})
    unset(_conflux_header_component_target)
endmacro()

function(conflux_header_support_component target export_name)
    if(NOT TARGET ${target})
        return()
    endif()
    list(APPEND CONFLUX_HEADER_INSTALL_TARGETS ${target})
    list(APPEND CONFLUX_PACKAGE_SUPPORT_COMPONENTS ${export_name})
    list(APPEND CONFLUX_PACKAGE_SUPPORT_TARGETS conflux::${export_name})
    list(APPEND CONFLUX_PACKAGE_ALL_COMPONENTS ${export_name})
    list(APPEND CONFLUX_PACKAGE_ALL_TARGETS conflux::${export_name})
    set(CONFLUX_HEADER_INSTALL_TARGETS "${CONFLUX_HEADER_INSTALL_TARGETS}" PARENT_SCOPE)
    set(CONFLUX_PACKAGE_SUPPORT_COMPONENTS "${CONFLUX_PACKAGE_SUPPORT_COMPONENTS}" PARENT_SCOPE)
    set(CONFLUX_PACKAGE_SUPPORT_TARGETS "${CONFLUX_PACKAGE_SUPPORT_TARGETS}" PARENT_SCOPE)
    set(CONFLUX_PACKAGE_ALL_COMPONENTS "${CONFLUX_PACKAGE_ALL_COMPONENTS}" PARENT_SCOPE)
    set(CONFLUX_PACKAGE_ALL_TARGETS "${CONFLUX_PACKAGE_ALL_TARGETS}" PARENT_SCOPE)
endfunction()

function(conflux_validate_header_impl_metadata)
    get_property(_targets GLOBAL PROPERTY CONFLUX_HEADER_IMPL_TARGETS)
    get_property(_components GLOBAL PROPERTY CONFLUX_HEADER_IMPL_COMPONENTS)
    get_property(_namespaced_targets GLOBAL PROPERTY CONFLUX_HEADER_IMPL_NAMESPACED_TARGETS)
    list(LENGTH _targets _target_count)
    list(LENGTH _components _component_count)
    list(LENGTH _namespaced_targets _namespaced_target_count)
    if(NOT _target_count EQUAL _component_count
            OR NOT _target_count EQUAL _namespaced_target_count)
        message(FATAL_ERROR
            "conflux: header implementation metadata lists are out of sync")
    endif()
    if(_target_count GREATER 0)
        set(_seen_targets)
        set(_seen_components)
        math(EXPR _last_index "${_target_count} - 1")
        foreach(_index RANGE 0 ${_last_index})
            list(GET _targets ${_index} _target)
            list(GET _components ${_index} _component)
            list(GET _namespaced_targets ${_index} _namespaced_target)
            if(_target IN_LIST _seen_targets)
                message(FATAL_ERROR
                    "conflux: header implementation target '${_target}' is listed more than once")
            endif()
            if(_component IN_LIST _seen_components)
                message(FATAL_ERROR
                    "conflux: header implementation component '${_component}' is listed more than once")
            endif()
            list(APPEND _seen_targets "${_target}")
            list(APPEND _seen_components "${_component}")
            if(NOT _component MATCHES "^header_impl_[A-Za-z0-9_]+$")
                message(FATAL_ERROR
                    "conflux: header implementation component '${_component}' must use the header_impl_ package namespace")
            endif()
            if(NOT _target STREQUAL "conflux_${_component}")
                message(FATAL_ERROR
                    "conflux: header implementation component '${_component}' must pair with target 'conflux_${_component}', got '${_target}'")
            endif()
            if(NOT _namespaced_target STREQUAL "conflux::${_component}")
                message(FATAL_ERROR
                    "conflux: header implementation component '${_component}' must pair with target 'conflux::${_component}', got '${_namespaced_target}'")
            endif()
        endforeach()
    endif()
endfunction()

conflux_header_public_component_by_export(core
    IMPLS conflux_header_impl_core
    COMPILE_DEFINITIONS CONFLUX_INTERFACE_HEADER=1)

conflux_header_public_component_by_export(types
    IMPLS conflux_header_impl_core)

set(CONFLUX_PACKAGE_SUPPORT_COMPONENTS)
set(CONFLUX_PACKAGE_SUPPORT_TARGETS)
set(CONFLUX_PACKAGE_ALL_COMPONENTS ${CONFLUX_PACKAGE_COMPONENTS})
set(CONFLUX_PACKAGE_ALL_TARGETS ${CONFLUX_PACKAGE_TARGETS})
conflux_header_support_component(conflux_headers headers)
conflux_validate_header_impl_metadata()
get_property(_conflux_header_impl_targets GLOBAL PROPERTY CONFLUX_HEADER_IMPL_TARGETS)
get_property(_conflux_header_impl_components GLOBAL PROPERTY CONFLUX_HEADER_IMPL_COMPONENTS)
get_property(_conflux_header_impl_namespaced_targets GLOBAL PROPERTY CONFLUX_HEADER_IMPL_NAMESPACED_TARGETS)
conflux_header_support_component(conflux_header_impl header_impl)
if(_conflux_header_impl_targets)
    list(APPEND CONFLUX_HEADER_INSTALL_TARGETS ${_conflux_header_impl_targets})
    list(APPEND CONFLUX_PACKAGE_SUPPORT_COMPONENTS ${_conflux_header_impl_components})
    list(APPEND CONFLUX_PACKAGE_SUPPORT_TARGETS ${_conflux_header_impl_namespaced_targets})
    list(APPEND CONFLUX_PACKAGE_ALL_COMPONENTS ${_conflux_header_impl_components})
    list(APPEND CONFLUX_PACKAGE_ALL_TARGETS ${_conflux_header_impl_namespaced_targets})
endif()
set(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS FALSE)
if(TARGET PkgConfig::LIBURING)
    set(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS TRUE)
endif()

if(CONFLUX_WANT_JSON)
    set(_conflux_json_compile_definitions)
    set(_conflux_json_links)
    if(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "XXHASH")
        list(APPEND _conflux_json_compile_definitions
            CONFLUX_JSON_HASH_PROVIDER_XXHASH=1)
        if(TARGET PkgConfig::XXHASH)
            list(APPEND _conflux_json_links PkgConfig::XXHASH)
        endif()
    elseif(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "INTERNAL")
        list(APPEND _conflux_json_compile_definitions
            CONFLUX_JSON_HASH_PROVIDER_INTERNAL=1)
    endif()
    conflux_header_public_component_by_export(json
        HPP_TOP_LEVEL
        IMPLS conflux_header_impl_json
        LINKS ${_conflux_json_links}
        COMPILE_DEFINITIONS ${_conflux_json_compile_definitions})
    unset(_conflux_json_compile_definitions)
    unset(_conflux_json_links)
endif()

if(CONFLUX_WANT_HTTP_CORE OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
    set(_conflux_http_component_options HPP_TOP_LEVEL)
    if(NOT CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS)
        set(_conflux_http_component_options NO_PACKAGE)
    endif()
    conflux_header_public_component_by_export(http
        ${_conflux_http_component_options}
        IMPLS
        conflux_header_impl_core
        conflux_header_impl_json
        conflux_header_impl_runtime
        conflux_header_impl_file_io_sync
        conflux_header_impl_file_map
        conflux_header_impl_file_io
        conflux_header_impl_socket_io
        conflux_header_impl_dns
        conflux_header_impl_http_core
        conflux_header_impl_http_server
        conflux_header_impl_http_static
        conflux_header_impl_http_client
        conflux_header_impl_http_proxy
        conflux_header_impl_templates
        LINKS PkgConfig::LIBURING)
    unset(_conflux_http_component_options)
endif()

conflux_header_public_component_by_export(file_io_sync
    IMPLS conflux_header_impl_file_io_sync)

if(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS)
    conflux_header_public_component_by_export(work
        HPP_TOP_LEVEL
        IMPLS
        conflux_header_impl_core
        conflux_header_impl_runtime
        conflux_header_impl_socket_io
        LINKS PkgConfig::LIBURING)
endif()

set(CONFLUX_HEADER_INSTALL_DB_COMPONENTS FALSE)
if(CONFLUX_HAS_DB STREQUAL "true" AND CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS)
    set(CONFLUX_HEADER_INSTALL_DB_COMPONENTS TRUE)
endif()

if(CONFLUX_HEADER_INSTALL_DB_COMPONENTS)
    set(_conflux_pg_links conflux_work)
    if(TARGET PkgConfig::LIBPQ)
        list(APPEND _conflux_pg_links PkgConfig::LIBPQ)
    endif()
    conflux_header_public_component_by_export(pg
        IMPLS conflux_header_impl_pg
        LINKS ${_conflux_pg_links})
    unset(_conflux_pg_links)
endif()

set(CONFLUX_PUBLIC_HPP_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/public-hpp/conflux")
conflux_register_header_public_hpp(config)
conflux_register_header_public_hpp(curated)
conflux_register_header_public_hpp(extended)
conflux_register_header_public_hpp(complete)
conflux_write_header_public_hpp_files("${CONFLUX_PUBLIC_HPP_DIR}")

conflux_add_header_component_smoke_targets()
conflux_add_header_link_smoke_targets()
conflux_add_header_examples_from_source_ids()
conflux_add_header_test_compile_targets()
conflux_add_header_compile_fail_tests()
conflux_add_header_benchmark_compile_targets()

conflux_any_target_links_item(CONFLUX_INSTALL_NEEDS_LIBURING
    PkgConfig::LIBURING
    ${CONFLUX_HEADER_INSTALL_TARGETS})

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

foreach(_target IN LISTS CONFLUX_HEADER_INSTALL_TARGETS)
    get_target_property(_export_name ${_target} EXPORT_NAME)
    install(TARGETS ${_target}
        EXPORT confluxTargets-${_export_name}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endforeach()
install(FILES
    ${CONFLUX_SRC_ROOT}/conflux/detail/discard.hxx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/detail
)
conflux_install_registered_public_headers()
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
)
foreach(_target IN LISTS CONFLUX_HEADER_INSTALL_TARGETS)
    get_target_property(_export_name ${_target} EXPORT_NAME)
    install(EXPORT confluxTargets-${_export_name}
        NAMESPACE conflux::
        FILE confluxTargets-${_export_name}.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
    )
endforeach()
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/ConfluxGeneratePackageMetadata.cmake")

if((CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
        AND CONFLUX_PACKAGE_SMOKE_PREFIX)
    conflux_add_package_config_install_tree_test(
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/package-smoke")
endif()

if((CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
        AND CONFLUX_RUN_INSTALL_TREE_SMOKE)
    conflux_add_install_tree_smoke_test("${CMAKE_CURRENT_SOURCE_DIR}")
endif()
