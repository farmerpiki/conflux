function(conflux_configure_interface_mode)
    set(CONFLUX_GENERATED_ROOT "${CMAKE_CURRENT_BINARY_DIR}/generated/bridge" CACHE PATH
        "Generated module/header bridge root")
    set(CONFLUX_GENERATED_INCLUDE_DIR "${CONFLUX_GENERATED_ROOT}/include" CACHE PATH
        "Generated conflux header include directory")
    set(CONFLUX_GENERATED_SOURCE_DIR "${CONFLUX_GENERATED_ROOT}/src" CACHE PATH
        "Generated conflux source overlay directory")
    set(CONFLUX_BRIDGE_MANIFEST "${CONFLUX_GENERATED_ROOT}/module_header_bridge_manifest.json" CACHE FILEPATH
        "Generated conflux header/module bridge manifest")
    set(CONFLUX_BRIDGE_CMAKE_FRAGMENT "${CONFLUX_GENERATED_ROOT}/ConfluxBridge.cmake" CACHE FILEPATH
        "Generated conflux bridge CMake helper fragment")
    set(CONFLUX_GENERATED_EXAMPLES_DIR "${CONFLUX_GENERATED_ROOT}/examples" CACHE PATH
        "Generated header-mode examples directory")
    set(CONFLUX_GENERATED_TESTS_DIR "${CONFLUX_GENERATED_ROOT}/tests" CACHE PATH
        "Generated header-mode tests directory")
    set(CONFLUX_GENERATED_BENCHMARKS_DIR "${CONFLUX_GENERATED_ROOT}/benchmarks" CACHE PATH
        "Generated header-mode benchmarks directory")

    set(CONFLUX_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src" PARENT_SCOPE)
    set(CONFLUX_GENERATED_INCLUDE_DIR "${CONFLUX_GENERATED_INCLUDE_DIR}" PARENT_SCOPE)
    set(CONFLUX_GENERATED_SOURCE_DIR "${CONFLUX_GENERATED_SOURCE_DIR}" PARENT_SCOPE)
    set(CONFLUX_GENERATED_EXAMPLES_DIR "${CONFLUX_GENERATED_EXAMPLES_DIR}" PARENT_SCOPE)
    set(CONFLUX_GENERATED_TESTS_DIR "${CONFLUX_GENERATED_TESTS_DIR}" PARENT_SCOPE)
    set(CONFLUX_GENERATED_BENCHMARKS_DIR "${CONFLUX_GENERATED_BENCHMARKS_DIR}" PARENT_SCOPE)

    if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE"
            AND CONFLUX_IMPORT_STD_ENABLED)
        return()
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_bridge_args
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/module_header_bridge.py"
        --src "${CMAKE_CURRENT_SOURCE_DIR}/src"
        --include-out "${CONFLUX_GENERATED_INCLUDE_DIR}"
        --manifest-out "${CONFLUX_BRIDGE_MANIFEST}"
        --cmake-fragment-out "${CONFLUX_BRIDGE_CMAKE_FRAGMENT}"
        --write)

    if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
        list(APPEND _bridge_args --warnings-as-errors)
    endif()

    if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
        list(APPEND _bridge_args
            --source-out "${CONFLUX_GENERATED_SOURCE_DIR}"
            --source-mode module-std-headers
            --consumer-mode module-std-headers)
        if(CONFLUX_BUILD_EXAMPLES)
            list(APPEND _bridge_args
                --examples-src "${CMAKE_CURRENT_SOURCE_DIR}/examples"
                --examples-out "${CONFLUX_GENERATED_EXAMPLES_DIR}")
        endif()
        if(CONFLUX_BUILD_TESTS)
            list(APPEND _bridge_args
                --tests-src "${CMAKE_CURRENT_SOURCE_DIR}/tests"
                --tests-out "${CONFLUX_GENERATED_TESTS_DIR}")
        endif()
        if(CONFLUX_BUILD_BENCHMARKS)
            list(APPEND _bridge_args
                --benchmarks-src "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
                --benchmarks-out "${CONFLUX_GENERATED_BENCHMARKS_DIR}")
        endif()
    elseif(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
        list(APPEND _bridge_args
            --source-out "${CONFLUX_GENERATED_SOURCE_DIR}"
            --source-mode header)
        if(CONFLUX_BUILD_EXAMPLES)
            list(APPEND _bridge_args
                --examples-src "${CMAKE_CURRENT_SOURCE_DIR}/examples"
                --examples-out "${CONFLUX_GENERATED_EXAMPLES_DIR}")
        endif()
        if(CONFLUX_BUILD_TESTS)
            list(APPEND _bridge_args
                --tests-src "${CMAKE_CURRENT_SOURCE_DIR}/tests"
                --tests-out "${CONFLUX_GENERATED_TESTS_DIR}")
        endif()
        if(CONFLUX_BUILD_BENCHMARKS)
            list(APPEND _bridge_args
                --benchmarks-src "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
                --benchmarks-out "${CONFLUX_GENERATED_BENCHMARKS_DIR}")
        endif()
    else()
        message(FATAL_ERROR "unknown CONFLUX_INTERFACE_MODE=${CONFLUX_INTERFACE_MODE}")
    endif()

    set(_bridge_input_roots
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/module_header_bridge.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(CONFLUX_BUILD_EXAMPLES)
        list(APPEND _bridge_input_roots "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    endif()
    if(CONFLUX_BUILD_TESTS)
        list(APPEND _bridge_input_roots "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    endif()
    if(CONFLUX_BUILD_BENCHMARKS)
        list(APPEND _bridge_input_roots "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks")
    endif()
    foreach(_bridge_input_root IN LISTS _bridge_input_roots)
        if(IS_DIRECTORY "${_bridge_input_root}")
            file(GLOB_RECURSE _bridge_inputs CONFIGURE_DEPENDS
                "${_bridge_input_root}/*")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_bridge_inputs})
        else()
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_bridge_input_root}")
        endif()
    endforeach()

    execute_process(
        COMMAND "${Python3_EXECUTABLE}" ${_bridge_args}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _bridge_rc
        COMMAND_ECHO STDOUT)
    if(NOT _bridge_rc EQUAL 0)
        message(FATAL_ERROR "conflux module/header bridge generation failed with exit code ${_bridge_rc}")
    endif()

    include("${CONFLUX_BRIDGE_CMAKE_FRAGMENT}")
    if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
        set(CONFLUX_SRC_ROOT "${CONFLUX_GENERATED_SOURCE_DIR}" PARENT_SCOPE)
    endif()
    if(DEFINED CONFLUX_BRIDGE_HEADER_IMPL_SOURCES
            AND NOT (CONFLUX_HEADER_USE_IMPORT_STD
                OR CONFLUX_HEADER_USE_IMPORT_STD_COMPAT
                OR CONFLUX_HEADER_USE_MODULE_IMPORTS))
        set_source_files_properties(${CONFLUX_BRIDGE_HEADER_IMPL_SOURCES}
            PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    endif()
    if(DEFINED CONFLUX_BRIDGE_HEADER_IMPL_SOURCES)
        set(CONFLUX_BRIDGE_HEADER_IMPL_SOURCES
            "${CONFLUX_BRIDGE_HEADER_IMPL_SOURCES}" PARENT_SCOPE)
    endif()
    if(DEFINED CONFLUX_BRIDGE_HEADER_IMPL_MODULES)
        set(CONFLUX_BRIDGE_HEADER_IMPL_MODULES
            "${CONFLUX_BRIDGE_HEADER_IMPL_MODULES}" PARENT_SCOPE)
    endif()

endfunction()

function(conflux_source_id_to_target_suffix out source_id)
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _target_suffix "${source_id}")
    set(${out} "${_target_suffix}" PARENT_SCOPE)
endfunction()

function(conflux_append_header_impl_sources_for_modules out module_regex)
    if(NOT DEFINED CONFLUX_BRIDGE_HEADER_IMPL_SOURCES
            OR NOT DEFINED CONFLUX_BRIDGE_HEADER_IMPL_MODULES)
        set(${out} "" PARENT_SCOPE)
        return()
    endif()

    list(LENGTH CONFLUX_BRIDGE_HEADER_IMPL_SOURCES _source_count)
    list(LENGTH CONFLUX_BRIDGE_HEADER_IMPL_MODULES _module_count)
    if(NOT _source_count EQUAL _module_count)
        message(FATAL_ERROR
            "conflux: generated header implementation source/module lists are out of sync")
    endif()

    set(_selected ${${out}})
    if(_source_count GREATER 0)
        math(EXPR _last_index "${_source_count} - 1")
        foreach(_index RANGE 0 ${_last_index})
            list(GET CONFLUX_BRIDGE_HEADER_IMPL_SOURCES ${_index} _source)
            list(GET CONFLUX_BRIDGE_HEADER_IMPL_MODULES ${_index} _module)
            if(_module MATCHES "${module_regex}")
                list(APPEND _selected "${_source}")
            endif()
        endforeach()
    endif()
    list(REMOVE_DUPLICATES _selected)
    set(${out} "${_selected}" PARENT_SCOPE)
endfunction()

function(conflux_resolve_source_id out source_id)
    if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE"
            OR (CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE" AND NOT CONFLUX_IMPORT_STD_ENABLED))
        if(source_id MATCHES "^examples/")
            string(REGEX REPLACE "^examples/" "" _rel "${source_id}")
            set(${out} "${CONFLUX_GENERATED_EXAMPLES_DIR}/${_rel}.cxx" PARENT_SCOPE)
            return()
        elseif(source_id MATCHES "^tests/")
            string(REGEX REPLACE "^tests/" "" _rel "${source_id}")
            set(${out} "${CONFLUX_GENERATED_TESTS_DIR}/${_rel}.cxx" PARENT_SCOPE)
            return()
        elseif(source_id MATCHES "^benchmarks/")
            string(REGEX REPLACE "^benchmarks/" "" _rel "${source_id}")
            set(${out} "${CONFLUX_GENERATED_BENCHMARKS_DIR}/${_rel}.cxx" PARENT_SCOPE)
            return()
        elseif(source_id MATCHES "^src/")
            string(REGEX REPLACE "^src/" "" _rel "${source_id}")
            set(${out} "${CONFLUX_GENERATED_SOURCE_DIR}/${_rel}.cxx" PARENT_SCOPE)
            return()
        endif()
    endif()
    set(${out} "${CMAKE_CURRENT_SOURCE_DIR}/${source_id}.cxx" PARENT_SCOPE)
endfunction()

function(conflux_apply_header_generated_build_policy target)
    # Generated header-mode implementation and consumer smoke targets are
    # build-system artifacts. They only need dependency scanning when this
    # header mode deliberately imports modules.
    if(CONFLUX_HEADER_USE_IMPORT_STD OR CONFLUX_HEADER_USE_IMPORT_STD_COMPAT OR CONFLUX_HEADER_USE_MODULE_IMPORTS)
        set_target_properties(${target} PROPERTIES CXX_SCAN_FOR_MODULES ON)
    else()
        set_target_properties(${target} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    endif()
    if(CONFLUX_HEADER_FAST_COMPILE)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-O0>
            $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Od>)
    endif()
endfunction()

function(conflux_apply_header_impl_common target)
    conflux_apply_header_generated_build_policy(${target})
    target_include_directories(${target} PRIVATE
        "${CONFLUX_GENERATED_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(CMAKE_CXX_STANDARD GREATER_EQUAL 26)
        target_compile_features(${target} PUBLIC cxx_std_26)
    else()
        target_compile_features(${target} PUBLIC cxx_std_23)
    endif()
    target_compile_definitions(${target} PRIVATE
        CONFLUX_HEADER_USE_IMPORT_STD=$<BOOL:${CONFLUX_HEADER_USE_IMPORT_STD}>
        CONFLUX_HEADER_USE_IMPORT_STD_COMPAT=$<BOOL:${CONFLUX_HEADER_USE_IMPORT_STD_COMPAT}>
        CONFLUX_HEADER_USE_MODULE_IMPORTS=$<BOOL:${CONFLUX_HEADER_USE_MODULE_IMPORTS}>
        CONFLUX_CPU_FEATURE_PROBES_RUNTIME=$<BOOL:${_conflux_cpu_feature_probes_runtime}>
        CONFLUX_SIMD_SELECTION_DIRECT=$<BOOL:${_conflux_simd_selection_direct}>
        CONFLUX_SIMD_SELECTION_RUNTIME=$<BOOL:${_conflux_simd_selection_runtime}>
        CONFLUX_HAS_TLS=$<BOOL:${CONFLUX_HAS_TLS}>
        CONFLUX_HAS_COMPRESS=$<BOOL:${CONFLUX_HAS_COMPRESS}>
        CONFLUX_HAS_ZLIB=$<BOOL:${CONFLUX_HAS_ZLIB}>
        CONFLUX_HAS_LIBDEFLATE=$<BOOL:${CONFLUX_HAS_LIBDEFLATE}>
        CONFLUX_HAS_ZLIB_NG=$<BOOL:${CONFLUX_HAS_ZLIB_NG}>
        CONFLUX_HAS_ISAL=$<BOOL:${CONFLUX_HAS_ISAL}>
        CONFLUX_HAS_BROTLI=$<BOOL:${CONFLUX_HAS_BROTLI}>
        CONFLUX_HAS_ZSTD=$<BOOL:${CONFLUX_HAS_ZSTD}>
        CONFLUX_HAS_HTTP2=$<BOOL:${CONFLUX_HAS_HTTP2}>
        CONFLUX_HAS_HTTP3=$<BOOL:${CONFLUX_HAS_HTTP3}>
        CONFLUX_HAS_JSON=$<BOOL:${CONFLUX_HAS_JSON}>
        CONFLUX_HAS_FILE_WATCH=$<BOOL:${CONFLUX_HAS_FILE_WATCH}>
        CONFLUX_HAS_TEMPLATES=$<BOOL:${CONFLUX_HAS_TEMPLATES}>
        CONFLUX_HAS_TEMPLATES_WATCH=$<BOOL:${CONFLUX_HAS_TEMPLATES_WATCH}>
        CONFLUX_HAS_METRICS=$<BOOL:${CONFLUX_HAS_METRICS}>
        CONFLUX_HAS_DB=$<BOOL:${CONFLUX_HAS_DB}>)
    conflux_apply_api_surface_definitions(${target} PRIVATE)
endfunction()

function(conflux_link_header_impl_hash_provider target)
    if(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "XXHASH")
        target_compile_definitions(${target} PRIVATE CONFLUX_JSON_HASH_PROVIDER_XXHASH=1)
        if(TARGET PkgConfig::XXHASH)
            target_link_libraries(${target} PUBLIC PkgConfig::XXHASH)
        endif()
    elseif(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "INTERNAL")
        target_compile_definitions(${target} PRIVATE CONFLUX_JSON_HASH_PROVIDER_INTERNAL=1)
    endif()
endfunction()

function(conflux_link_header_impl_liburing target)
    if(TARGET PkgConfig::LIBURING)
        target_link_libraries(${target} PUBLIC PkgConfig::LIBURING)
    endif()
endfunction()

function(conflux_link_header_impl_tls_deps target)
    foreach(_target IN ITEMS OpenSSL::SSL OpenSSL::Crypto PkgConfig::NGHTTP2 PkgConfig::NGTCP2 PkgConfig::NGTCP2_CRYPTO_OSSL PkgConfig::NGHTTP3)
        if(TARGET ${_target})
            target_link_libraries(${target} PUBLIC ${_target})
        endif()
    endforeach()
endfunction()

function(conflux_link_header_impl_db_deps target)
    if(TARGET PkgConfig::LIBPQ)
        target_link_libraries(${target} PUBLIC PkgConfig::LIBPQ)
    endif()
endfunction()

function(conflux_link_header_impl_crypto_deps target)
    foreach(_target IN ITEMS OpenSSL::SSL OpenSSL::Crypto PkgConfig::ARGON2)
        if(TARGET ${_target})
            target_link_libraries(${target} PUBLIC ${_target})
        endif()
    endforeach()
endfunction()

function(conflux_register_header_impl_target target export_name)
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
    set_property(GLOBAL APPEND PROPERTY CONFLUX_HEADER_IMPL_TARGETS ${target})
    set_property(GLOBAL APPEND PROPERTY CONFLUX_HEADER_IMPL_COMPONENTS ${export_name})
    set_property(GLOBAL APPEND PROPERTY CONFLUX_HEADER_IMPL_NAMESPACED_TARGETS conflux::${export_name})
endfunction()

function(conflux_register_header_package_component target export_name)
    set(options HPP_TOP_LEVEL)
    set(one_value_args)
    set(multi_value_args MODULE_PREFIXES)
    cmake_parse_arguments(CONFLUX_HEADER_COMPONENT
        "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(NOT TARGET ${target})
        return()
    endif()
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
    if(NOT CONFLUX_HEADER_COMPONENT_MODULE_PREFIXES)
        set(CONFLUX_HEADER_COMPONENT_MODULE_PREFIXES conflux.${export_name})
    endif()

    foreach(_list IN ITEMS
            CONFLUX_HEADER_INSTALL_TARGETS
            CONFLUX_PACKAGE_COMPONENTS
            CONFLUX_PACKAGE_ALL_COMPONENTS)
        set(_values ${${_list}})
        if(_list STREQUAL "CONFLUX_HEADER_INSTALL_TARGETS")
            list(APPEND _values ${target})
        else()
            list(APPEND _values ${export_name})
        endif()
        set(${_list} "${_values}" PARENT_SCOPE)
    endforeach()

    foreach(_list IN ITEMS
            CONFLUX_PACKAGE_TARGETS
            CONFLUX_PACKAGE_ALL_TARGETS)
        set(_values ${${_list}})
        list(APPEND _values conflux::${export_name})
        set(${_list} "${_values}" PARENT_SCOPE)
    endforeach()

    if(CONFLUX_HEADER_COMPONENT_MODULE_PREFIXES)
        conflux_register_header_public_surface(${target}
            MODULE_PREFIXES ${CONFLUX_HEADER_COMPONENT_MODULE_PREFIXES})
    endif()
    set(_hpp_path "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/${export_name}.hxx")
    if(EXISTS "${_hpp_path}")
        conflux_register_header_public_hpp("${export_name}")
        if(CONFLUX_HEADER_COMPONENT_HPP_TOP_LEVEL)
            set_property(GLOBAL APPEND PROPERTY
                CONFLUX_HEADER_PUBLIC_HPP_TOP_LEVEL_NAMES
                "${export_name}")
        endif()
    elseif(CONFLUX_HEADER_COMPONENT_HPP_TOP_LEVEL)
        message(FATAL_ERROR
            "conflux: HPP_TOP_LEVEL requested for '${export_name}', but "
            "'${_hpp_path}' does not exist")
    endif()
endfunction()

function(conflux_register_header_public_hpp name)
    set_property(GLOBAL APPEND PROPERTY
        CONFLUX_HEADER_PUBLIC_HPP_NAMES "${name}")
endfunction()

function(conflux_write_header_public_hpp_files out_dir)
    file(MAKE_DIRECTORY "${out_dir}")
    file(WRITE "${out_dir}/conflux.hpp" "#pragma once\n#include <conflux.hxx>\n")

    get_property(_hpp_names GLOBAL PROPERTY CONFLUX_HEADER_PUBLIC_HPP_NAMES)
    get_property(_top_level_names GLOBAL PROPERTY
        CONFLUX_HEADER_PUBLIC_HPP_TOP_LEVEL_NAMES)

    list(LENGTH _hpp_names _hpp_name_count)
    if(_hpp_name_count GREATER 0)
        math(EXPR _last_index "${_hpp_name_count} - 1")
        foreach(_index RANGE 0 ${_last_index})
            list(GET _hpp_names ${_index} _hpp_name)
            file(WRITE "${out_dir}/${_hpp_name}.hpp"
                "#pragma once\n#include <conflux/${_hpp_name}.hxx>\n")
            if(_hpp_name IN_LIST _top_level_names)
                file(APPEND "${out_dir}/conflux.hpp"
                    "#include <conflux/${_hpp_name}.hpp>\n")
            endif()
        endforeach()
    endif()
endfunction()

function(conflux_define_header_impl_component target export_name module_regex)
    set(_sources)
    conflux_append_header_impl_sources_for_modules(_sources "${module_regex}")
    if(NOT _sources)
        return()
    endif()
    add_library(${target} STATIC ${_sources})
    add_library(conflux::${export_name} ALIAS ${target})
    conflux_apply_header_impl_common(${target})
    conflux_register_header_impl_target(${target} ${export_name})
endfunction()

function(conflux_link_existing_header_impls target)
    foreach(_impl IN LISTS ARGN)
        if(TARGET ${_impl})
            target_link_libraries(${target} INTERFACE ${_impl})
        endif()
    endforeach()
endfunction()

function(conflux_link_existing_header_impls_private target)
    foreach(_impl IN LISTS ARGN)
        if(TARGET ${_impl})
            target_link_libraries(${target} PRIVATE ${_impl})
        endif()
    endforeach()
endfunction()

function(conflux_define_header_impl_targets)
    if(NOT CONFLUX_HEADER_INTERFACE_WITH_SOURCES
            OR NOT DEFINED CONFLUX_BRIDGE_HEADER_IMPL_SOURCES)
        return()
    endif()

    set_property(GLOBAL PROPERTY CONFLUX_HEADER_IMPL_TARGETS "")
    set_property(GLOBAL PROPERTY CONFLUX_HEADER_IMPL_COMPONENTS "")
    set_property(GLOBAL PROPERTY CONFLUX_HEADER_IMPL_NAMESPACED_TARGETS "")

    conflux_define_header_impl_component(conflux_header_impl_core header_impl_core
        "^conflux\.(types|utils)($|[.:])")

    if(CONFLUX_WANT_JSON OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
        conflux_define_header_impl_component(conflux_header_impl_json header_impl_json
            "^conflux\.json($|[.:])")
        if(TARGET conflux_header_impl_json)
            target_sources(conflux_header_impl_json PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/src/json_libc.cxx")
            conflux_link_header_impl_hash_provider(conflux_header_impl_json)
        endif()
    endif()

    if(CONFLUX_NEEDS_RUNTIME)
        conflux_define_header_impl_component(conflux_header_impl_runtime header_impl_runtime
            "^conflux\.(uring($|[.:])|work($|[.:])|net\.io_buffer($|[.:])|net\.cancel($|[.:]))")
        if(TARGET conflux_header_impl_runtime)
            conflux_link_header_impl_liburing(conflux_header_impl_runtime)
        endif()
    endif()

    if(CONFLUX_WANT_FILE_IO_SYNC)
        conflux_define_header_impl_component(conflux_header_impl_file_io_sync header_impl_file_io_sync
            "^conflux\.file_io_sync$")
    endif()
    if(CONFLUX_WANT_FILE_MAP)
        conflux_define_header_impl_component(conflux_header_impl_file_map header_impl_file_map
            "^conflux\.file_map$")
    endif()
    if(CONFLUX_WANT_FILE_IO)
        conflux_define_header_impl_component(conflux_header_impl_file_io header_impl_file_io
            "^conflux\.file_io($|[.:])")
        if(TARGET conflux_header_impl_file_io)
            conflux_link_header_impl_liburing(conflux_header_impl_file_io)
        endif()
    endif()
    if(CONFLUX_WANT_SOCKET_IO)
        conflux_define_header_impl_component(conflux_header_impl_socket_io header_impl_socket_io
            "^conflux\.socket_io($|[.:])")
        if(TARGET conflux_header_impl_socket_io)
            conflux_link_header_impl_liburing(conflux_header_impl_socket_io)
        endif()
    endif()
    if(CONFLUX_WANT_DNS)
        conflux_define_header_impl_component(conflux_header_impl_dns header_impl_dns
            "^conflux\.net\.dns($|[.:])")
        if(TARGET conflux_header_impl_dns)
            conflux_link_header_impl_liburing(conflux_header_impl_dns)
        endif()
    endif()
    if(CONFLUX_WANT_PROCESS)
        conflux_define_header_impl_component(conflux_header_impl_process header_impl_process
            "^conflux\.process($|[.:])")
        if(TARGET conflux_header_impl_process)
            conflux_link_header_impl_liburing(conflux_header_impl_process)
        endif()
    endif()
    if(CONFLUX_WANT_CRYPTO)
        conflux_define_header_impl_component(conflux_header_impl_crypto header_impl_crypto
            "^conflux\.crypto($|[.:])")
        if(TARGET conflux_header_impl_crypto)
            conflux_link_header_impl_crypto_deps(conflux_header_impl_crypto)
        endif()
    endif()

    if(CONFLUX_WANT_HTTP_CORE OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
        conflux_define_header_impl_component(conflux_header_impl_http_core header_impl_http_core
            "^conflux\.(http($|:problem)|net\.(app($|[.:])|config($|[.:])|http\.types|http\.request|http\.server_types|http\.json|http1|http_parse_helpers|response($|[.:])|router($|[.:])))")
        if(TARGET conflux_header_impl_http_core)
            conflux_link_existing_header_impls(conflux_header_impl_http_core
                conflux_header_impl_runtime)
        endif()
    endif()
    if(CONFLUX_WANT_HTTP_SERVER)
        conflux_define_header_impl_component(conflux_header_impl_http_server header_impl_http_server
            "^conflux\.net\.http_server($|[.:])")
        if(TARGET conflux_header_impl_http_server)
            conflux_link_header_impl_liburing(conflux_header_impl_http_server)
            conflux_link_header_impl_tls_deps(conflux_header_impl_http_server)
        endif()
    endif()
    if(CONFLUX_WANT_HTTP_STATIC OR CONFLUX_HTTP_ROUTER_STACK_REQUESTED OR CONFLUX_WANT_HTTP_SERVER)
        conflux_define_header_impl_component(conflux_header_impl_http_static header_impl_http_static
            "^conflux\.net\.(router_static|http\.static_async)($|[.:])")
        if(TARGET conflux_header_impl_http_static)
            conflux_link_existing_header_impls(conflux_header_impl_http_static
                conflux_header_impl_runtime)
            conflux_link_header_impl_liburing(conflux_header_impl_http_static)
        endif()
    endif()
    if(CONFLUX_WANT_HTTP_CLIENT OR CONFLUX_HTTP_CLIENT_STACK_REQUESTED)
        conflux_define_header_impl_component(conflux_header_impl_http_client header_impl_http_client
            "^conflux\.net\.(async_client|client)($|[.:])")
        if(TARGET conflux_header_impl_http_client)
            conflux_link_header_impl_liburing(conflux_header_impl_http_client)
            conflux_link_header_impl_tls_deps(conflux_header_impl_http_client)
        endif()
    endif()
    if(CONFLUX_WANT_HTTP_PROXY)
        conflux_define_header_impl_component(conflux_header_impl_http_proxy header_impl_http_proxy
            "^conflux\.net\.proxy($|[.:])")
        if(TARGET conflux_header_impl_http_proxy)
            conflux_link_header_impl_liburing(conflux_header_impl_http_proxy)
            conflux_link_header_impl_tls_deps(conflux_header_impl_http_proxy)
        endif()
    endif()
    if(CONFLUX_WANT_TEMPLATES)
        conflux_define_header_impl_component(conflux_header_impl_templates header_impl_templates
            "^conflux\.templates($|[.:])")
    endif()
    if(CONFLUX_HAS_DB STREQUAL "true")
        conflux_define_header_impl_component(conflux_header_impl_pg header_impl_pg
            "^conflux\.pg($|[.:])")
        if(TARGET conflux_header_impl_pg)
            conflux_link_header_impl_liburing(conflux_header_impl_pg)
            conflux_link_header_impl_db_deps(conflux_header_impl_pg)
        endif()
    endif()
    if(CONFLUX_WANT_SMTP)
        conflux_define_header_impl_component(conflux_header_impl_smtp header_impl_smtp
            "^conflux\.net\.smtp($|[.:])")
        if(TARGET conflux_header_impl_smtp)
            conflux_link_header_impl_liburing(conflux_header_impl_smtp)
            conflux_link_header_impl_tls_deps(conflux_header_impl_smtp)
        endif()
    endif()

    get_property(_impl_targets GLOBAL PROPERTY CONFLUX_HEADER_IMPL_TARGETS)
    if(_impl_targets)
        add_library(conflux_header_impl INTERFACE)
        add_library(conflux::header_impl ALIAS conflux_header_impl)
        set_target_properties(conflux_header_impl PROPERTIES EXPORT_NAME header_impl)
        target_link_libraries(conflux_header_impl INTERFACE ${_impl_targets})
    else()
        message(STATUS
            "conflux: HEADER_INTERFACE_WITH_SOURCES selected no implementation sources")
    endif()
endfunction()

function(conflux_add_executable_from_id target source_id)
    conflux_resolve_source_id(_source "${source_id}")
    add_executable(${target} "${_source}")
    set_property(TARGET ${target} PROPERTY CONFLUX_SOURCE_ID "${source_id}")
    conflux_apply_header_generated_build_policy(${target})
endfunction()

function(conflux_add_object_from_id target source_id)
    conflux_resolve_source_id(_source "${source_id}")
    add_library(${target} OBJECT "${_source}")
    set_property(TARGET ${target} PROPERTY CONFLUX_SOURCE_ID "${source_id}")
    conflux_apply_header_generated_build_policy(${target})
endfunction()

function(conflux_add_header_interface_target)
    add_library(conflux_headers INTERFACE)
    add_library(conflux::headers ALIAS conflux_headers)
    set_target_properties(conflux_headers PROPERTIES EXPORT_NAME headers)
    target_include_directories(conflux_headers INTERFACE
        "$<BUILD_INTERFACE:${CONFLUX_GENERATED_INCLUDE_DIR}>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    if(CMAKE_CXX_STANDARD GREATER_EQUAL 26)
        target_compile_features(conflux_headers INTERFACE cxx_std_26)
    else()
        target_compile_features(conflux_headers INTERFACE cxx_std_23)
    endif()
    target_compile_definitions(conflux_headers INTERFACE
        CONFLUX_INTERFACE_HEADER=1
        CONFLUX_HEADER_USE_IMPORT_STD=$<BOOL:${CONFLUX_HEADER_USE_IMPORT_STD}>
        CONFLUX_HEADER_USE_IMPORT_STD_COMPAT=$<BOOL:${CONFLUX_HEADER_USE_IMPORT_STD_COMPAT}>
        CONFLUX_HEADER_USE_MODULE_IMPORTS=$<BOOL:${CONFLUX_HEADER_USE_MODULE_IMPORTS}>
        CONFLUX_CPU_FEATURE_PROBES_RUNTIME=$<BOOL:${_conflux_cpu_feature_probes_runtime}>
        CONFLUX_SIMD_SELECTION_DIRECT=$<BOOL:${_conflux_simd_selection_direct}>
        CONFLUX_SIMD_SELECTION_RUNTIME=$<BOOL:${_conflux_simd_selection_runtime}>
        CONFLUX_HAS_TLS=$<BOOL:${CONFLUX_HAS_TLS}>
        CONFLUX_HAS_COMPRESS=$<BOOL:${CONFLUX_HAS_COMPRESS}>
        CONFLUX_HAS_ZLIB=$<BOOL:${CONFLUX_HAS_ZLIB}>
        CONFLUX_HAS_LIBDEFLATE=$<BOOL:${CONFLUX_HAS_LIBDEFLATE}>
        CONFLUX_HAS_ZLIB_NG=$<BOOL:${CONFLUX_HAS_ZLIB_NG}>
        CONFLUX_HAS_ISAL=$<BOOL:${CONFLUX_HAS_ISAL}>
        CONFLUX_HAS_BROTLI=$<BOOL:${CONFLUX_HAS_BROTLI}>
        CONFLUX_HAS_ZSTD=$<BOOL:${CONFLUX_HAS_ZSTD}>
        CONFLUX_HAS_HTTP2=$<BOOL:${CONFLUX_HAS_HTTP2}>
        CONFLUX_HAS_HTTP3=$<BOOL:${CONFLUX_HAS_HTTP3}>
        CONFLUX_HAS_JSON=$<BOOL:${CONFLUX_HAS_JSON}>
        CONFLUX_HAS_FILE_WATCH=$<BOOL:${CONFLUX_HAS_FILE_WATCH}>
        CONFLUX_HAS_TEMPLATES=$<BOOL:${CONFLUX_HAS_TEMPLATES}>
        CONFLUX_HAS_TEMPLATES_WATCH=$<BOOL:${CONFLUX_HAS_TEMPLATES_WATCH}>
        CONFLUX_HAS_METRICS=$<BOOL:${CONFLUX_HAS_METRICS}>
        CONFLUX_HAS_DB=$<BOOL:${CONFLUX_HAS_DB}>)
    conflux_apply_api_surface_definitions(conflux_headers INTERFACE)
    # Keep conflux::headers dependency-free. Component-specific provider
    # definitions and libraries are attached to requestable component targets
    # so core-only downstream consumers do not resolve JSON-only packages.

    conflux_define_header_impl_targets()
endfunction()

function(conflux_link_header_http_impls target)
    conflux_link_existing_header_impls_private(${target}
        conflux_header_impl_core
        conflux_header_impl_runtime
        conflux_header_impl_file_io
        conflux_header_impl_file_map
        conflux_header_impl_socket_io
        conflux_header_impl_dns
        conflux_header_impl_http_core
        conflux_header_impl_http_server
        conflux_header_impl_http_static)
endfunction()

function(conflux_add_header_example_from_id target source_id)
    set(options HTTP_IMPLS)
    set(one_value_args)
    set(multi_value_args IMPLS)
    cmake_parse_arguments(CONFLUX_HEADER_EXAMPLE
        "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(CONFLUX_HEADER_EXAMPLE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "conflux_add_header_example_from_id(${target}) got unexpected arguments: "
            "${CONFLUX_HEADER_EXAMPLE_UNPARSED_ARGUMENTS}")
    endif()
    if(CONFLUX_HEADER_INTERFACE_WITH_SOURCES AND CONFLUX_HEADER_LINK_EXAMPLES)
        conflux_add_executable_from_id(${target} "${source_id}")
    else()
        conflux_add_object_from_id(${target} "${source_id}")
    endif()
    target_link_libraries(${target} PRIVATE conflux_headers)
    if(CONFLUX_HEADER_LINK_EXAMPLES)
        conflux_link_existing_header_impls_private(${target} conflux_header_impl_core)
        if(CONFLUX_HEADER_EXAMPLE_HTTP_IMPLS)
            conflux_link_header_http_impls(${target})
        endif()
        if(CONFLUX_HEADER_EXAMPLE_IMPLS)
            conflux_link_existing_header_impls_private(${target}
                ${CONFLUX_HEADER_EXAMPLE_IMPLS})
        endif()
    endif()
    set_property(GLOBAL APPEND PROPERTY CONFLUX_HEADER_EXAMPLE_TARGETS ${target})
endfunction()

function(conflux_add_header_link_smoke_targets)
    if(NOT CONFLUX_HEADER_LINK_SMOKE
            OR NOT CONFLUX_HEADER_INTERFACE_WITH_SOURCES
            OR NOT CONFLUX_WANT_HTTP_SERVER)
        return()
    endif()

    set(_smoke_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/header-link-smoke")
    file(MAKE_DIRECTORY "${_smoke_dir}")
    file(WRITE "${_smoke_dir}/http.cxx" [[
#include <conflux/net/http/response.hxx>
#include <conflux/net/http/types.hxx>

int main() {
    auto response = Response::text("ok");
    return response.status == kHttpOk && response.text_body() == "ok" ? 0 : 1;
}
]])

    add_executable(conflux_header_link_smoke_http "${_smoke_dir}/http.cxx")
    conflux_apply_header_generated_build_policy(conflux_header_link_smoke_http)
    target_link_libraries(conflux_header_link_smoke_http PRIVATE conflux_headers)
    conflux_link_existing_header_impls_private(conflux_header_link_smoke_http
        conflux_header_impl_core)
    if(CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
        add_test(NAME header/link-smoke-http COMMAND conflux_header_link_smoke_http)
    endif()
endfunction()

function(conflux_register_header_public_surface target)
    set(options)
    set(one_value_args)
    set(multi_value_args MODULE_PREFIXES)
    cmake_parse_arguments(CONFLUX_HEADER_SURFACE
        "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(NOT CONFLUX_HEADER_SURFACE_MODULE_PREFIXES)
        return()
    endif()
    if(TARGET ${target})
        set_property(GLOBAL APPEND PROPERTY
            CONFLUX_ACTIVE_PUBLIC_HEADER_MODULES
            ${CONFLUX_HEADER_SURFACE_MODULE_PREFIXES})
        set_property(GLOBAL APPEND PROPERTY
            CONFLUX_ACTIVE_PUBLIC_HEADER_TARGETS
            ${target})
    endif()
endfunction()

function(conflux_link_header_smoke_surface target surface_target)
    if(TARGET ${surface_target})
        target_link_libraries(${target} PRIVATE ${surface_target})
    else()
        target_link_libraries(${target} PRIVATE conflux_headers)
    endif()
endfunction()

function(conflux_add_header_component_smoke_targets)
    set(_smoke_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/header-component-smoke")
    file(MAKE_DIRECTORY "${_smoke_dir}")

    set(_public_smoke_dir "${_smoke_dir}/public-includes")
    set(_public_smoke_fragment "${_smoke_dir}/public-includes.cmake")
    set(_public_smoke_skip_args)
    set(_public_smoke_active_modules)
    get_property(_active_public_header_modules GLOBAL PROPERTY
        CONFLUX_ACTIVE_PUBLIC_HEADER_MODULES)
    foreach(_active_module IN LISTS _active_public_header_modules)
        list(APPEND _public_smoke_active_modules
            --active-module "${_active_module}")
    endforeach()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-public-header-include-smoke.py"
                --manifest "${CONFLUX_BRIDGE_MANIFEST}"
                --out-dir "${_public_smoke_dir}"
                --cmake-fragment "${_public_smoke_fragment}"
                ${_public_smoke_skip_args}
                ${_public_smoke_active_modules}
        RESULT_VARIABLE _public_smoke_result)
    if(NOT _public_smoke_result EQUAL 0)
        message(FATAL_ERROR "conflux: public header include smoke generation failed")
    endif()
    include("${_public_smoke_fragment}")
    if(CONFLUX_PUBLIC_HEADER_SMOKE_SOURCES)
        add_library(conflux_header_smoke_public_includes EXCLUDE_FROM_ALL OBJECT
            ${CONFLUX_PUBLIC_HEADER_SMOKE_SOURCES})
        conflux_apply_header_generated_build_policy(conflux_header_smoke_public_includes)
        target_link_libraries(conflux_header_smoke_public_includes PRIVATE conflux_headers)
        get_property(_active_public_header_targets GLOBAL PROPERTY
            CONFLUX_ACTIVE_PUBLIC_HEADER_TARGETS)
        if(_active_public_header_targets)
            list(REMOVE_DUPLICATES _active_public_header_targets)
            target_link_libraries(conflux_header_smoke_public_includes PRIVATE
                ${_active_public_header_targets})
        endif()
        conflux_apply_header_smoke_warnings(conflux_header_smoke_public_includes)
        message(STATUS "conflux: generated ${CONFLUX_PUBLIC_HEADER_SMOKE_COUNT} public header include smoke TUs")
    endif()

    add_custom_target(conflux_header_smoke_public_hygiene
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-public-header-hygiene.py"
                --manifest "${CONFLUX_BRIDGE_MANIFEST}"
                --include-dir "${CONFLUX_GENERATED_INCLUDE_DIR}"
                ${_public_smoke_skip_args}
        COMMENT "Checking generated public header hygiene")

    file(WRITE "${_smoke_dir}/core.cxx" "#include <conflux/types.hxx>\nint main() { return 0; }\n")
    add_executable(conflux_header_smoke_core "${_smoke_dir}/core.cxx")
    conflux_apply_header_generated_build_policy(conflux_header_smoke_core)
    conflux_link_header_smoke_surface(conflux_header_smoke_core conflux_core)
    conflux_apply_header_smoke_warnings(conflux_header_smoke_core)

    if(CONFLUX_WANT_HTTP_SERVER)
        foreach(_surface IN ITEMS curated extended complete selected)
            if(_surface STREQUAL "selected")
                set(_surface_header "conflux.hxx")
                set(_surface_target "conflux_header_smoke_api_surface_selected")
            else()
                set(_surface_header "conflux/${_surface}.hxx")
                set(_surface_target "conflux_header_smoke_api_surface_${_surface}")
            endif()
            file(WRITE "${_smoke_dir}/api_surface_${_surface}.cxx"
                "#include <${_surface_header}>\nint main() { return 0; }\n")
            add_library(${_surface_target} OBJECT "${_smoke_dir}/api_surface_${_surface}.cxx")
            conflux_apply_header_generated_build_policy(${_surface_target})
            target_link_libraries(${_surface_target} PRIVATE conflux_headers)
            conflux_apply_header_smoke_warnings(${_surface_target})
        endforeach()
    endif()

    if(CONFLUX_WANT_JSON)
        file(WRITE "${_smoke_dir}/json.cxx" "#include <conflux/json.hxx>\nint main() { return 0; }\n")
        add_executable(conflux_header_smoke_json "${_smoke_dir}/json.cxx")
        conflux_apply_header_generated_build_policy(conflux_header_smoke_json)
        conflux_link_header_smoke_surface(conflux_header_smoke_json conflux_json)
        conflux_apply_header_smoke_warnings(conflux_header_smoke_json)
    endif()

    if(CONFLUX_NEEDS_RUNTIME)
        file(WRITE "${_smoke_dir}/runtime.cxx" "#include <conflux/work.hxx>\nint main() { return 0; }\n")
        add_executable(conflux_header_smoke_runtime "${_smoke_dir}/runtime.cxx")
        conflux_apply_header_generated_build_policy(conflux_header_smoke_runtime)
        conflux_link_header_smoke_surface(conflux_header_smoke_runtime conflux_work)
        conflux_apply_header_smoke_warnings(conflux_header_smoke_runtime)
    endif()

    if(CONFLUX_HEADER_INSTALL_DB_COMPONENTS)
        file(WRITE "${_smoke_dir}/pg.cxx" "#include <conflux/pg/connection.hxx>\nint main() { return 0; }\n")
        add_library(conflux_header_smoke_pg OBJECT "${_smoke_dir}/pg.cxx")
        conflux_apply_header_generated_build_policy(conflux_header_smoke_pg)
        if(TARGET conflux_pg)
            target_link_libraries(conflux_header_smoke_pg PRIVATE conflux_pg)
        else()
            message(FATAL_ERROR
                "conflux: DB header smoke requires the conflux_pg component target")
        endif()
        conflux_apply_header_smoke_warnings(conflux_header_smoke_pg)
    endif()
endfunction()


function(conflux_collect_public_module_smoke_sources out)
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

function(conflux_add_public_module_import_smoke_target)
    if(NOT CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
        return()
    endif()

    set(options)
    set(one_value_args)
    set(multi_value_args TARGETS SKIP_MODULE SKIP_PREFIX)
    cmake_parse_arguments(CONFLUX_MODULE_SMOKE
        "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT CONFLUX_MODULE_SMOKE_TARGETS)
        return()
    endif()

    conflux_collect_public_module_smoke_sources(
        _public_module_sources ${CONFLUX_MODULE_SMOKE_TARGETS})
    if(NOT _public_module_sources)
        return()
    endif()

    if(NOT Python3_EXECUTABLE)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    endif()

    set(_smoke_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/module-import-smoke")
    set(_source_list "${_smoke_dir}/public-module-sources.txt")
    set(_cmake_fragment "${_smoke_dir}/public-module-imports.cmake")
    file(MAKE_DIRECTORY "${_smoke_dir}")
    file(WRITE "${_source_list}" "")
    foreach(_source IN LISTS _public_module_sources)
        file(APPEND "${_source_list}" "${_source}\n")
    endforeach()

    set(_skip_args)
    foreach(_module IN LISTS CONFLUX_MODULE_SMOKE_SKIP_MODULE)
        list(APPEND _skip_args --skip-module "${_module}")
    endforeach()
    foreach(_prefix IN LISTS CONFLUX_MODULE_SMOKE_SKIP_PREFIX)
        list(APPEND _skip_args --skip-prefix "${_prefix}")
    endforeach()

    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-public-module-import-smoke.py"
                --source-list "${_source_list}"
                --out-dir "${_smoke_dir}/sources"
                --cmake-fragment "${_cmake_fragment}"
                ${_skip_args}
        RESULT_VARIABLE _module_smoke_result)
    if(NOT _module_smoke_result EQUAL 0)
        message(FATAL_ERROR "conflux: public module import smoke generation failed")
    endif()
    include("${_cmake_fragment}")
    if(NOT CONFLUX_PUBLIC_MODULE_SMOKE_SOURCES)
        return()
    endif()

    add_library(conflux_module_smoke_public_imports EXCLUDE_FROM_ALL OBJECT
        ${CONFLUX_PUBLIC_MODULE_SMOKE_SOURCES})
    target_compile_features(conflux_module_smoke_public_imports PRIVATE cxx_std_23)
    target_link_libraries(conflux_module_smoke_public_imports PRIVATE
        conflux_options
        ${CONFLUX_MODULE_SMOKE_TARGETS})
    message(STATUS
        "conflux: generated ${CONFLUX_PUBLIC_MODULE_SMOKE_COUNT} public module import smoke TUs")
endfunction()

function(conflux_apply_header_smoke_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wcast-align
        -Wconversion
        -Wdisabled-optimization
        -Wdouble-promotion
        -Wextra
        -Wformat=2
        -Wimplicit-fallthrough
        -Wnon-virtual-dtor
        -Wnull-dereference
        -Wold-style-cast
        -Woverloaded-virtual
        -Wpedantic
        -Wredundant-decls
        -Wsign-conversion
        -Wunused)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE
            -Wmisleading-indentation
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
            -Wmissing-noreturn
            -Wmissing-format-attribute
            -Wno-global-module
            -Wno-missing-field-initializers)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target} PRIVATE
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
            -Wno-unqualified-std-cast-call
            -Wno-missing-designated-field-initializers)
    endif()
endfunction()

function(conflux_add_header_examples_from_source_ids)
    if(NOT CONFLUX_BUILD_EXAMPLES)
        return()
    endif()
    set_property(GLOBAL PROPERTY CONFLUX_HEADER_EXAMPLE_TARGETS "")
    set(_conflux_examples_manifest "${CMAKE_CURRENT_BINARY_DIR}/conflux_examples_manifest.txt")
    file(WRITE "${_conflux_examples_manifest}" "Conflux examples manifest\n\n")
    set(_conflux_has_header_http_facade FALSE)
    if(TARGET conflux_net_http)
        set(_conflux_has_header_http_facade TRUE)
    endif()

    if(_conflux_has_header_http_facade AND CONFLUX_WANT_HTTP_SERVER)
        conflux_add_header_example_from_id(conflux_quickstart_hello examples/quickstart/hello HTTP_IMPLS)
        conflux_add_header_example_from_id(conflux_quickstart_middleware examples/quickstart/middleware HTTP_IMPLS)
        conflux_add_header_example_from_id(conflux_quickstart_sse examples/quickstart/sse HTTP_IMPLS)
        conflux_add_header_example_from_id(conflux_quickstart_static_files examples/quickstart/static_files HTTP_IMPLS)
        conflux_add_header_example_from_id(conflux_quickstart_websocket examples/quickstart/websocket HTTP_IMPLS)
        conflux_add_header_example_from_id(conflux_quickstart_openapi examples/quickstart/openapi
            HTTP_IMPLS
            IMPLS conflux_header_impl_json)
        conflux_add_header_example_from_id(conflux_hello examples/hello
            HTTP_IMPLS
            IMPLS conflux_header_impl_json)
        conflux_add_header_example_from_id(conflux_middleware examples/middleware
            HTTP_IMPLS
            IMPLS conflux_header_impl_json)
        conflux_add_header_example_from_id(conflux_sse examples/sse HTTP_IMPLS)
        conflux_add_header_example_from_id(conflux_static examples/static
            HTTP_IMPLS
            IMPLS conflux_header_impl_json)
        conflux_add_header_example_from_id(conflux_forms examples/forms HTTP_IMPLS)
        conflux_add_header_example_from_id(conflux_gzip examples/gzip
            HTTP_IMPLS
            IMPLS conflux_header_impl_json)
        conflux_add_header_example_from_id(conflux_dual examples/advanced/dual
            HTTP_IMPLS
            IMPLS conflux_header_impl_http_client conflux_header_impl_http_proxy)
        conflux_add_header_example_from_id(conflux_http_observability_example examples/advanced/http_observability HTTP_IMPLS)
        conflux_add_header_example_from_id(conflux_production_showcase_example examples/advanced/production_showcase
            HTTP_IMPLS
            IMPLS conflux_header_impl_json conflux_header_impl_templates)
        conflux_add_header_example_from_id(conflux_http_policy_stack_example examples/advanced/http_policy_stack HTTP_IMPLS)
        conflux_add_header_example_from_id(conflux_vhost_openapi_example examples/advanced/vhost_openapi HTTP_IMPLS)
    endif()

    if(_conflux_has_header_http_facade AND CONFLUX_WANT_HTTP_CLIENT)
        conflux_add_header_example_from_id(conflux_http_client examples/http_client
            HTTP_IMPLS
            IMPLS conflux_header_impl_http_client conflux_header_impl_http_proxy)
        conflux_add_header_example_from_id(conflux_http_client_builder_example examples/advanced/http_client_builder
            HTTP_IMPLS
            IMPLS conflux_header_impl_http_client conflux_header_impl_http_proxy)
    endif()
    if(CONFLUX_WANT_PROCESS)
        conflux_add_header_example_from_id(conflux_process_run_example examples/advanced/process_run
            IMPLS conflux_header_impl_process)
    endif()
    if(CONFLUX_WANT_CRYPTO)
        conflux_add_header_example_from_id(conflux_crypto_sealing_example examples/advanced/crypto_sealing
            IMPLS conflux_header_impl_crypto)
    endif()
    if(CONFLUX_WANT_RUNTIME)
        conflux_add_header_example_from_id(conflux_work_join_all_example examples/advanced/work_join_all
            IMPLS conflux_header_impl_runtime)
    endif()
    if(CONFLUX_WANT_JSON)
        conflux_add_header_example_from_id(conflux_json_example examples/advanced/json
            IMPLS conflux_header_impl_json)
        conflux_add_header_example_from_id(conflux_json_config_example examples/advanced/json_config
            IMPLS conflux_header_impl_json)
        conflux_add_header_example_from_id(conflux_json_stream_ingest_example examples/advanced/json_stream_ingest
            IMPLS conflux_header_impl_json)
        conflux_add_header_example_from_id(conflux_json_diagnostics_example examples/advanced/json_diagnostics
            IMPLS conflux_header_impl_json)
        conflux_add_header_example_from_id(conflux_json_transform_example examples/advanced/json_transform
            IMPLS conflux_header_impl_json)
    endif()

    if(_conflux_has_header_http_facade AND CONFLUX_WANT_HTTP_SERVER AND CONFLUX_HAS_JSON)
        conflux_add_header_example_from_id(conflux_quickstart_json_crud examples/quickstart/json_crud
            HTTP_IMPLS
            IMPLS conflux_header_impl_json)
        conflux_add_header_example_from_id(conflux_http_explicit_offload_example examples/advanced/explicit_offload
            HTTP_IMPLS
            IMPLS conflux_header_impl_json conflux_header_impl_runtime)
        conflux_add_header_example_from_id(conflux_http_client_json_example examples/advanced/http_client_json
            HTTP_IMPLS
            IMPLS conflux_header_impl_json conflux_header_impl_http_client conflux_header_impl_http_proxy)
        conflux_add_header_example_from_id(conflux_api_typed_json_example examples/advanced/manual_json_members
            HTTP_IMPLS
            IMPLS conflux_header_impl_json)
    endif()
    if(_conflux_has_header_http_facade AND CONFLUX_WANT_HTTP_CORE AND CONFLUX_HAS_JSON)
        conflux_add_header_example_from_id(conflux_custom_json_provider_example examples/advanced/custom_json_provider
            IMPLS conflux_header_impl_json)
    endif()

    if(_conflux_has_header_http_facade AND CONFLUX_WANT_HTTP_SERVER AND CONFLUX_JSON_REFLECT)
        conflux_add_header_example_from_id(conflux_quickstart_json_reflect_crud examples/quickstart/json_reflect_crud
            HTTP_IMPLS
            IMPLS conflux_header_impl_json)
    endif()

    if(CONFLUX_WANT_TEMPLATES)
        conflux_add_header_example_from_id(conflux_template_pages_example examples/advanced/template_pages
            IMPLS conflux_header_impl_templates)
    endif()

    if(_conflux_has_header_http_facade AND CONFLUX_HAS_HTTP3)
        conflux_add_header_example_from_id(conflux_h3_probe examples/advanced/h3_probe HTTP_IMPLS)
        conflux_add_header_example_from_id(conflux_h3_server examples/advanced/h3_server HTTP_IMPLS)
    endif()

    if(_conflux_has_header_http_facade AND CONFLUX_WANT_HTTP_SERVER AND CONFLUX_HAS_DB STREQUAL "true")
        conflux_add_header_example_from_id(conflux_quickstart_postgres examples/quickstart/postgres_json
            HTTP_IMPLS
            IMPLS conflux_header_impl_json conflux_header_impl_runtime conflux_header_impl_socket_io conflux_header_impl_pg)
    endif()
    if(CONFLUX_HAS_DB STREQUAL "true")
        conflux_add_header_example_from_id(conflux_db_basic examples/advanced/db_basic
            IMPLS conflux_header_impl_runtime conflux_header_impl_socket_io conflux_header_impl_pg)
        conflux_add_header_example_from_id(conflux_db_pool examples/advanced/db_pool
            IMPLS conflux_header_impl_runtime conflux_header_impl_socket_io conflux_header_impl_pg)
        conflux_add_header_example_from_id(conflux_advanced_postgres examples/advanced/postgres
            IMPLS conflux_header_impl_runtime conflux_header_impl_socket_io conflux_header_impl_pg)
    endif()

    if(CONFLUX_JSON_REFLECT)
        conflux_add_header_example_from_id(conflux_json_reflect_example examples/advanced/json_reflect
            IMPLS conflux_header_impl_json)
    endif()

    add_custom_target(conflux_header_examples)
    add_custom_target(conflux_examples)
    get_property(_conflux_header_example_targets GLOBAL PROPERTY CONFLUX_HEADER_EXAMPLE_TARGETS)
    if(_conflux_header_example_targets)
        foreach(_conflux_header_example_target IN LISTS _conflux_header_example_targets)
            file(APPEND "${_conflux_examples_manifest}"
                "${_conflux_header_example_target}: built - header example target\n")
        endforeach()
        add_dependencies(conflux_header_examples ${_conflux_header_example_targets})
        add_dependencies(conflux_examples ${_conflux_header_example_targets})
    endif()
    if(CONFLUX_BUILD_TESTS)
        add_test(NAME examples/compile
            COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                    --target conflux_examples --config "$<CONFIG>")
        set_tests_properties(examples/compile PROPERTIES
            LABELS "examples;build;header"
            RUN_SERIAL TRUE)
    endif()
endfunction()

function(conflux_add_header_consumer_compile_target aggregate target_prefix enabled)
    if(NOT enabled)
        return()
    endif()
    set(_conflux_header_consumer_targets)
    foreach(_source_id IN LISTS ARGN)
        conflux_source_id_to_target_suffix(_target_suffix "${_source_id}")
        set(_target "${target_prefix}_${_target_suffix}")
        if(aggregate STREQUAL "conflux_header_benchmarks" AND CONFLUX_HEADER_INTERFACE_WITH_SOURCES)
            conflux_add_executable_from_id(${_target} "${_source_id}")
        else()
            conflux_add_object_from_id(${_target} "${_source_id}")
        endif()
        target_link_libraries(${_target} PRIVATE conflux_headers)
        if(aggregate STREQUAL "conflux_header_tests")
            target_compile_definitions(${_target} PRIVATE
                ASSERT_PROBE_BIN="/nonexistent/conflux_header_assert_probe"
                CONFLUX_TESTING=1
                JSONTESTSUITE_DIR="/nonexistent/conflux_header_json_testsuite")
        endif()
        list(APPEND _conflux_header_consumer_targets ${_target})
    endforeach()
    add_custom_target(${aggregate})
    if(_conflux_header_consumer_targets)
        add_dependencies(${aggregate} ${_conflux_header_consumer_targets})
    endif()
    if(aggregate STREQUAL "conflux_header_tests" AND TARGET "${target_prefix}_tests_template_test")
        add_test(NAME template/header-compile
            COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                    --target "${target_prefix}_tests_template_test" --config "$<CONFIG>")
        set_tests_properties(template/header-compile PROPERTIES
            LABELS "template;build;header"
            RUN_SERIAL TRUE)
    endif()
endfunction()

function(conflux_add_header_test_compile_targets)
    set(_conflux_header_test_source_ids
        tests/aes_gcm_openssl_compare
        tests/assert_probe_support
        tests/auth_rate_limit_test
        tests/auth_secret_config_test
        tests/caps_test
        tests/config_test
        tests/cq_overflow_test
        tests/crypto_test
        tests/db_integration_test
        tests/db_test
        tests/direct_accept_sqe_test
        tests/direct_slot_pool_test
        tests/dns_codec_test
        tests/dns_resolver_test
        tests/external_support
        tests/file_io_http_e2e
        tests/file_io_sync_test
        tests/file_io_test
        tests/h2_external
        tests/h3_external
        tests/http3_test
        tests/http_core_test
        tests/http_e2e
        tests/http_facade_api_snapshot
        tests/http_facade_import_smoke
        tests/http_facade_test
        tests/http_json_test
        tests/http_overflow_stress_test
        tests/http_request_assert_probe
        tests/http_response_test
        tests/http_server_helpers_test
        tests/incremental_buf_ring_assert_probe
        tests/incremental_buf_ring_test
        tests/json_boundary_test
        tests/json_conformance_external
        tests/json_file_test
        tests/json_test
        tests/json_testsuite_gate
        tests/jwt_test
        tests/libcurl_external
        tests/owned_path_flow_test
        tests/password_hash_test
        tests/poll_first_auto_test
        tests/process_test
        tests/recv_bundle_assert_probe
        tests/recv_bundle_e2e_test
        tests/recv_bundle_test
        tests/resource_limits_test
        tests/ring_resize_test
        tests/send_zc_lifecycle_test
        tests/smoke
        tests/smtp_test
        tests/socket_task_ring_test
        tests/support
        tests/tcp_listener_test
        tests/template_test
        tests/tls_external
        tests/udp_test
        tests/uring_flow_test
        tests/utils_test
        tests/work_api_snapshot
        tests/work_carrier_phase2_test
        tests/work_carrier_phase3_test
        tests/work_carrier_phase4_test
        tests/work_carrier_phase5_test
        tests/work_carrier_phase6_test
        tests/work_carrier_streams_test
        tests/work_carrier_test
        tests/work_carrier_timer_test
        tests/work_root_test
        tests/work_test)
    if(CONFLUX_JSON_REFLECT)
        list(APPEND _conflux_header_test_source_ids
            tests/json_reflection_main
            tests/json_reflection_test)
    endif()
    conflux_add_header_consumer_compile_target(
        conflux_header_tests
        conflux_header_test
        "${CONFLUX_BUILD_TESTS}"
        ${_conflux_header_test_source_ids})
endfunction()

function(conflux_add_header_compile_fail_test name source_id)
    if(NOT CONFLUX_BUILD_TESTS)
        return()
    endif()
    set(_expected ${ARGN})
    if(NOT _expected)
        message(FATAL_ERROR "conflux_add_header_compile_fail_test requires at least one expected diagnostic")
    endif()
    conflux_source_id_to_target_suffix(_target_suffix "${source_id}")
    set(_target "conflux_header_compile_fail_${_target_suffix}")
    conflux_resolve_source_id(_source "${source_id}")
    add_library(${_target} EXCLUDE_FROM_ALL OBJECT "${_source}")
    set_property(TARGET ${_target} PROPERTY CONFLUX_SOURCE_ID "${source_id}")
    target_link_libraries(${_target} PRIVATE conflux_headers)
    add_test(NAME "${name}"
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                ${_target}
                ${_expected})
    set_tests_properties("${name}" PROPERTIES
        LABELS "http;compile-fail;header"
        RUN_SERIAL TRUE)
endfunction()

function(conflux_add_header_compile_fail_tests)
    if(NOT CONFLUX_BUILD_TESTS)
        return()
    endif()
    conflux_add_header_compile_fail_test(
        http-facade-header/compile-fail-raw-string
        tests/http_facade_compile_fail_raw_string
        "HTTP app handlers must not return raw strings"
        "use http::text(...)"
        "http::html(...)")
    conflux_add_header_compile_fail_test(
        http-facade-header/compile-fail-raw-string-extractor
        tests/http_facade_compile_fail_raw_string_extractor
        "HTTP app handlers must not return raw strings"
        "use http::text(...)"
        "http::html(...)")
    conflux_add_header_compile_fail_test(
        http-facade-header/compile-fail-json-codec
        tests/http_facade_compile_fail_json_codec
        "http::Json<T> responses require T to be serializable"
        "add JsonCodec<T>, JsonMembers<T>, or reflection JSON support")
    conflux_add_header_compile_fail_test(
        http-facade-header/compile-fail-route-policy-internal
        tests/http_facade_compile_fail_route_policy_internal
        "no type named 'AppRouteRateLimit' in namespace 'conflux::http'")
    conflux_add_header_compile_fail_test(
        http-facade-header/compile-fail-middleware-concept-alias
        tests/http_facade_compile_fail_middleware_concept_alias
        "no template named 'Middleware' in namespace 'conflux::http'")
    conflux_add_header_compile_fail_test(
        http-facade-header/compile-fail-router-member
        tests/http_facade_compile_fail_router_member
        "conflux_http_facade_unexpected_router_member_visible")
    conflux_add_header_compile_fail_test(
        http-facade-header/compile-fail-route-infos-member
        tests/http_facade_compile_fail_route_infos_member
        "conflux_http_facade_unexpected_route_infos_member_visible")

        if(CONFLUX_WANT_HTTP_SERVER)
            conflux_add_header_compile_fail_test(
                api-surface/header-curated-hides-iouring
                tests/header_api_surface_curated_compile_fail_iouring
                "conflux::uring")
            conflux_add_header_compile_fail_test(
                api-surface/header-curated-hides-blocking-file
                tests/header_api_surface_curated_compile_fail_file
                "conflux_api_surface_curated_unexpected_file_helper_visible")
            conflux_add_header_compile_fail_test(
                api-surface/header-extended-hides-iouring
                tests/header_api_surface_extended_compile_fail_iouring
                "conflux::uring")
            conflux_add_header_compile_fail_test(
                api-surface/header-complete-hides-direct-slot-pool
                tests/header_api_surface_complete_compile_fail_direct_slot_pool
                "DirectSlotPool")
            conflux_add_header_compile_fail_test(
                api-surface/header-http-facade-offload-free
                tests/header_http_facade_compile_fail_offload_free
                "no member named 'offload' in namespace 'conflux::http'")
            conflux_add_header_compile_fail_test(
                api-surface/header-http-facade-openapi-handler-free
                tests/header_http_facade_compile_fail_openapi_handler_free
                "no member named 'openapi_handler' in namespace 'conflux::http'")
            conflux_add_header_compile_fail_test(
                api-surface/header-http-facade-file-free
                tests/header_http_facade_compile_fail_file_free
                "conflux_http_facade_unexpected_file_helper_visible")
        conflux_add_header_compile_fail_test(
            api-surface/header-http-facade-route-infos-member
            tests/header_http_facade_compile_fail_route_infos_member
            "conflux_http_facade_header_unexpected_route_infos_member_visible")
    endif()
endfunction()

function(conflux_add_header_benchmark_compile_targets)
    set(_conflux_header_benchmark_source_ids
        benchmarks/bench_common
        benchmarks/crypto_bench
        benchmarks/file_copy_coro_bench
        benchmarks/http_adversarial_bench
        benchmarks/http_app_path_bench
        benchmarks/http_server_bench
        benchmarks/http_server_concurrency_bench
        benchmarks/http11_representative_bench
        benchmarks/join_all_N_bench
        benchmarks/json_bench
        benchmarks/json_direct_struct_bench
        benchmarks/json_storage_bench
        benchmarks/kernel_state_synthetic_bench
        benchmarks/router_bench
        benchmarks/socket_raw_bench
        benchmarks/slow_consumer_backpressure_bench
        benchmarks/storage_read_bench
        benchmarks/static_strategy_matrix_bench
        benchmarks/task_cancellation_bench
        benchmarks/task_chain_composition_bench
        benchmarks/task_creation_bench
        benchmarks/synthetic_cqe_coro_bench
        benchmarks/tcp_increment_coro_bench
        benchmarks/template_bench
        benchmarks/uring_completion_bench
        benchmarks/uring_timeout_bench
        benchmarks/work_bench
        benchmarks/work_compile_bench
        benchmarks/workpool_enqueue_dequeue_bench)
    if(_conflux_cpu_feature_probes_runtime AND CONFLUX_WANT_CRYPTO)
        list(APPEND _conflux_header_benchmark_source_ids
            benchmarks/cpu_dispatch_impl_bench)
    endif()
    if(CONFLUX_HAS_TLS STREQUAL "true")
        list(APPEND _conflux_header_benchmark_source_ids
            benchmarks/tls_tcp_increment_coro_bench
            benchmarks/tls_mem_bio_bench)
    endif()
    if(CONFLUX_HAS_HTTP2 STREQUAL "true")
        list(APPEND _conflux_header_benchmark_source_ids
            benchmarks/http2_server_bench)
    endif()
    if(CONFLUX_HAS_DB STREQUAL "true")
        list(APPEND _conflux_header_benchmark_source_ids
            benchmarks/db_coro_bench
            benchmarks/db_params_bench
            benchmarks/db_pipeline_bench
            benchmarks/db_protocol_synthetic_bench)
    endif()
    if(CONFLUX_JSON_REFLECT)
        list(APPEND _conflux_header_benchmark_source_ids
            benchmarks/json_reflect_bench)
    endif()
    if(CONFLUX_EXPERIMENTAL_SEND_ZC)
        list(APPEND _conflux_header_benchmark_source_ids
            benchmarks/send_zc_bench)
    endif()
    conflux_add_header_consumer_compile_target(
        conflux_header_benchmarks
        conflux_header_benchmark
        "${CONFLUX_BUILD_BENCHMARKS}"
        ${_conflux_header_benchmark_source_ids})
    if(TARGET conflux_header_benchmark_benchmarks_db_protocol_synthetic_bench
            AND TARGET conflux_pg)
        target_link_libraries(
            conflux_header_benchmark_benchmarks_db_protocol_synthetic_bench
            PRIVATE conflux_pg)
    endif()
endfunction()
