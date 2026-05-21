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
    set(CONFLUX_MOCK_LIBURING_ROOT "${CONFLUX_GENERATED_ROOT}/mock_liburing" CACHE PATH
        "Generated compile-only mock liburing root")
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
    set(CONFLUX_MOCK_LIBURING_ROOT "${CONFLUX_MOCK_LIBURING_ROOT}" PARENT_SCOPE)

    if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
        return()
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_bridge_args
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/module_header_bridge.py"
        --src "${CMAKE_CURRENT_SOURCE_DIR}/src"
        --include-out "${CONFLUX_GENERATED_INCLUDE_DIR}"
        --manifest-out "${CONFLUX_BRIDGE_MANIFEST}"
        --cmake-fragment-out "${CONFLUX_BRIDGE_CMAKE_FRAGMENT}"
        --warnings-as-errors
        --write)

    if(CONFLUX_USE_MOCK_LIBURING)
        list(APPEND _bridge_args
            --mock-liburing-out "${CONFLUX_MOCK_LIBURING_ROOT}")
    endif()

    if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
        list(APPEND _bridge_args
            --source-out "${CONFLUX_GENERATED_SOURCE_DIR}"
            --source-mode header
            --examples-src "${CMAKE_CURRENT_SOURCE_DIR}/examples"
            --examples-out "${CONFLUX_GENERATED_EXAMPLES_DIR}")
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

    execute_process(
        COMMAND "${Python3_EXECUTABLE}" ${_bridge_args}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _bridge_rc
        COMMAND_ECHO STDOUT)
    if(NOT _bridge_rc EQUAL 0)
        message(FATAL_ERROR "conflux module/header bridge generation failed with exit code ${_bridge_rc}")
    endif()

    include("${CONFLUX_BRIDGE_CMAKE_FRAGMENT}")
    if(DEFINED CONFLUX_BRIDGE_HEADER_IMPL_SOURCES)
        set_source_files_properties(${CONFLUX_BRIDGE_HEADER_IMPL_SOURCES}
            PROPERTIES CXX_SCAN_FOR_MODULES OFF)
        set(CONFLUX_BRIDGE_HEADER_IMPL_SOURCES
            "${CONFLUX_BRIDGE_HEADER_IMPL_SOURCES}" PARENT_SCOPE)
    endif()
    if(DEFINED CONFLUX_BRIDGE_HEADER_IMPL_MODULES)
        set(CONFLUX_BRIDGE_HEADER_IMPL_MODULES
            "${CONFLUX_BRIDGE_HEADER_IMPL_MODULES}" PARENT_SCOPE)
    endif()

    if(CONFLUX_USE_MOCK_LIBURING)
        if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
            set(ENV{PKG_CONFIG_PATH} "${CONFLUX_MOCK_LIBURING_ROOT}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
        else()
            set(ENV{PKG_CONFIG_PATH} "${CONFLUX_MOCK_LIBURING_ROOT}/lib/pkgconfig")
        endif()
    endif()

endfunction()

function(conflux_bridge_link_header_dependencies target scope)
    foreach(_target IN ITEMS
            PkgConfig::LIBURING
            PkgConfig::XXHASH
            OpenSSL::SSL
            OpenSSL::Crypto
            ZLIB::ZLIB
            PkgConfig::BROTLI
            PkgConfig::ZSTD
            PkgConfig::LIBDEFLATE
            PkgConfig::ZLIB_NG
            PkgConfig::LIBISAL
            PkgConfig::NGHTTP2
            PkgConfig::NGTCP2
            PkgConfig::NGTCP2_CRYPTO_OSSL
            PkgConfig::NGHTTP3
            PkgConfig::LIBPQ
            PkgConfig::ARGON2)
        if(TARGET ${_target})
            target_link_libraries(${target} ${scope} ${_target})
        endif()
    endforeach()
    if(UNIX)
        target_link_libraries(${target} ${scope} ${CMAKE_DL_LIBS})
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

function(conflux_select_header_impl_sources out)
    set(_selected)

    conflux_append_header_impl_sources_for_modules(_selected
        "^conflux\\.(types|utils)$")

    if(CONFLUX_WANT_CRYPTO)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.crypto($|\\.)")
    endif()
    if(CONFLUX_WANT_JSON OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.json\\.boundary$")
    endif()
    if(CONFLUX_WANT_JSON)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.json($|\\.)")
    endif()
    if(CONFLUX_WANT_JSON_FILE)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.json\\.file$")
    endif()
    if(CONFLUX_WANT_FILE_IO_SYNC)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.file_io\\.sync$")
    endif()
    if(CONFLUX_WANT_FILE_MAP)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.file_io\\.map$")
    endif()
    if(CONFLUX_NEEDS_RUNTIME)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.(uring($|\\.)|work($|\\.)|net\\.io_buffer|net\\.cancel($|:))")
    endif()
    if(CONFLUX_WANT_FILE_IO)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.file_io($|\\.)")
    endif()
    if(CONFLUX_WANT_FILE_WATCH)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.file_watch$")
    endif()
    if(CONFLUX_WANT_SOCKET_IO)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.socket_io($|[.:])")
    endif()
    if(CONFLUX_WANT_DNS)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.net\\.dns($|:)")
    endif()
    if(CONFLUX_WANT_PROCESS)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.process$")
    endif()
    if(CONFLUX_WANT_TEMPLATES)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.templates$")
    endif()
    if(CONFLUX_WANT_TEMPLATES_WATCH)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.templates\\.watch$")
    endif()
    if(CONFLUX_WANT_HTTP_CORE OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.net\\.(config|http\\.types|http\\.request|http\\.server_types|http\\.json|http1|http_parse_helpers)$")
    endif()
    if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED OR CONFLUX_HTTP_CLIENT_STACK_REQUESTED OR CONFLUX_WANT_HTTP_SERVER)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.(http($|:)|net\\.(app($|\\.)|auth|cache_control|client($|\\.)|compress|cookie_signing|cors|csrf|etag|forwarded|http($|\\.)|http_server($|:)|ip_filter|jwt|metrics|openid|password_hash|proxy($|:)|rate_limit|realtime|redirect|request_id|response|response_cache|router($|:)|router_static|security|smtp|static($|\\.)|structured_log|tls|tracing|trailing_slash|vhost|openapi))")
    endif()
    if(CONFLUX_HAS_DB STREQUAL "true")
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.db($|\\.)")
    endif()
    if(CONFLUX_WANT_SMTP)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux\\.net\\.smtp$")
    endif()
    if(CONFLUX_WANT_HTTP_SERVER)
        conflux_append_header_impl_sources_for_modules(_selected
            "^conflux$")
    endif()

    list(REMOVE_DUPLICATES _selected)
    set(${out} "${_selected}" PARENT_SCOPE)
endfunction()

function(conflux_resolve_source_id out source_id)
    if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
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

function(conflux_add_executable_from_id target source_id)
    conflux_resolve_source_id(_source "${source_id}")
    add_executable(${target} "${_source}")
    set_property(TARGET ${target} PROPERTY CONFLUX_SOURCE_ID "${source_id}")
endfunction()

function(conflux_add_object_from_id target source_id)
    conflux_resolve_source_id(_source "${source_id}")
    add_library(${target} OBJECT "${_source}")
    set_property(TARGET ${target} PROPERTY CONFLUX_SOURCE_ID "${source_id}")
endfunction()

function(conflux_add_header_interface_target)
    add_library(conflux_headers INTERFACE)
    add_library(conflux::headers ALIAS conflux_headers)
    set_target_properties(conflux_headers PROPERTIES EXPORT_NAME headers)
    target_include_directories(conflux_headers INTERFACE
        "$<BUILD_INTERFACE:${CONFLUX_GENERATED_INCLUDE_DIR}>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    if(CONFLUX_USE_MOCK_LIBURING)
        target_include_directories(conflux_headers INTERFACE
            "$<BUILD_INTERFACE:${CONFLUX_MOCK_LIBURING_ROOT}/include>")
    endif()
    if(CMAKE_CXX_STANDARD GREATER_EQUAL 26)
        target_compile_features(conflux_headers INTERFACE cxx_std_26)
    else()
        target_compile_features(conflux_headers INTERFACE cxx_std_23)
    endif()
    target_compile_definitions(conflux_headers INTERFACE
        CONFLUX_INTERFACE_HEADER=1
        CONFLUX_HEADER_USE_IMPORT_STD=0
        CONFLUX_HEADER_USE_IMPORT_STD_COMPAT=0
        CONFLUX_HEADER_USE_MODULE_IMPORTS=0
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
    if(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "XXHASH")
        target_compile_definitions(conflux_headers INTERFACE CONFLUX_JSON_HASH_PROVIDER_XXHASH=1)
        if(TARGET PkgConfig::XXHASH)
            target_link_libraries(conflux_headers INTERFACE PkgConfig::XXHASH)
        endif()
    elseif(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "INTERNAL")
        target_compile_definitions(conflux_headers INTERFACE CONFLUX_JSON_HASH_PROVIDER_INTERNAL=1)
    endif()

    if(CONFLUX_HEADER_INTERFACE_WITH_SOURCES AND DEFINED CONFLUX_BRIDGE_HEADER_IMPL_SOURCES)
        conflux_select_header_impl_sources(CONFLUX_SELECTED_HEADER_IMPL_SOURCES)
        if(NOT CONFLUX_SELECTED_HEADER_IMPL_SOURCES)
            message(STATUS
                "conflux: HEADER_INTERFACE_WITH_SOURCES selected no implementation sources")
        endif()
    endif()

    if(CONFLUX_HEADER_INTERFACE_WITH_SOURCES AND CONFLUX_SELECTED_HEADER_IMPL_SOURCES)
        add_library(conflux_header_impl STATIC ${CONFLUX_SELECTED_HEADER_IMPL_SOURCES})
        add_library(conflux::header_impl ALIAS conflux_header_impl)
        set_target_properties(conflux_header_impl PROPERTIES EXPORT_NAME header_impl)
        target_include_directories(conflux_header_impl PRIVATE
            "${CONFLUX_GENERATED_INCLUDE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        if(CONFLUX_USE_MOCK_LIBURING)
            target_include_directories(conflux_header_impl PRIVATE
                "${CONFLUX_MOCK_LIBURING_ROOT}/include")
        elseif(TARGET PkgConfig::LIBURING)
            target_link_libraries(conflux_header_impl PUBLIC PkgConfig::LIBURING)
        endif()
        conflux_bridge_link_header_dependencies(conflux_header_impl PUBLIC)
        if(CMAKE_CXX_STANDARD GREATER_EQUAL 26)
            target_compile_features(conflux_header_impl PUBLIC cxx_std_26)
        else()
            target_compile_features(conflux_header_impl PUBLIC cxx_std_23)
        endif()
        target_compile_definitions(conflux_header_impl PRIVATE
            CONFLUX_HEADER_USE_IMPORT_STD=0
            CONFLUX_HEADER_USE_IMPORT_STD_COMPAT=0
            CONFLUX_HEADER_USE_MODULE_IMPORTS=0
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
        target_link_libraries(conflux_headers INTERFACE conflux_header_impl)
    endif()
endfunction()

function(conflux_add_header_example_from_id target source_id)
    if(CONFLUX_HEADER_INTERFACE_WITH_SOURCES)
        conflux_add_executable_from_id(${target} "${source_id}")
    else()
        conflux_add_object_from_id(${target} "${source_id}")
    endif()
    target_link_libraries(${target} PRIVATE conflux_headers)
    set_property(GLOBAL APPEND PROPERTY CONFLUX_HEADER_EXAMPLE_TARGETS ${target})
endfunction()

function(conflux_add_header_component_smoke_targets)
    set(_smoke_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/header-component-smoke")
    file(MAKE_DIRECTORY "${_smoke_dir}")

    file(WRITE "${_smoke_dir}/core.cxx" "#include <conflux/types.hxx>\nint main() { return 0; }\n")
    add_executable(conflux_header_smoke_core "${_smoke_dir}/core.cxx")
    target_link_libraries(conflux_header_smoke_core PRIVATE conflux_headers)
    conflux_apply_header_smoke_warnings(conflux_header_smoke_core)

    if(CONFLUX_WANT_JSON)
        file(WRITE "${_smoke_dir}/json.cxx" "#include <conflux/json.hxx>\nint main() { return 0; }\n")
        add_executable(conflux_header_smoke_json "${_smoke_dir}/json.cxx")
        target_link_libraries(conflux_header_smoke_json PRIVATE conflux_headers)
        conflux_apply_header_smoke_warnings(conflux_header_smoke_json)
    endif()

    if(CONFLUX_NEEDS_RUNTIME)
        file(WRITE "${_smoke_dir}/runtime.cxx" "#include <conflux/work.hxx>\nint main() { return 0; }\n")
        add_executable(conflux_header_smoke_runtime "${_smoke_dir}/runtime.cxx")
        target_link_libraries(conflux_header_smoke_runtime PRIVATE conflux_headers)
        conflux_apply_header_smoke_warnings(conflux_header_smoke_runtime)
    endif()

    if(CONFLUX_HAS_DB STREQUAL "true")
        file(WRITE "${_smoke_dir}/pg.cxx" "#include <conflux/pg/types.hxx>\nint main() { return 0; }\n")
        add_executable(conflux_header_smoke_pg "${_smoke_dir}/pg.cxx")
        target_link_libraries(conflux_header_smoke_pg PRIVATE conflux_headers)
        conflux_apply_header_smoke_warnings(conflux_header_smoke_pg)
    endif()
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
    function(conflux_note_header_example TARGET_NAME STATUS REASON)
        file(APPEND "${_conflux_examples_manifest}" "${TARGET_NAME}: ${STATUS} - ${REASON}\n")
    endfunction()

    conflux_add_header_example_from_id(conflux_quickstart_hello examples/quickstart/hello)
    conflux_note_header_example(conflux_quickstart_hello built "preview HTTP example")
    conflux_add_header_example_from_id(conflux_quickstart_middleware examples/quickstart/middleware)
    conflux_note_header_example(conflux_quickstart_middleware built "preview HTTP example")
    conflux_add_header_example_from_id(conflux_quickstart_sse examples/quickstart/sse)
    conflux_note_header_example(conflux_quickstart_sse built "preview HTTP example")
    conflux_add_header_example_from_id(conflux_quickstart_static_files examples/quickstart/static_files)
    conflux_note_header_example(conflux_quickstart_static_files built "preview HTTP example")
    conflux_add_header_example_from_id(conflux_quickstart_websocket examples/quickstart/websocket)
    conflux_note_header_example(conflux_quickstart_websocket built "preview HTTP example")
    conflux_add_header_example_from_id(conflux_quickstart_openapi examples/quickstart/openapi)
    conflux_note_header_example(conflux_quickstart_openapi built "preview HTTP example")
    conflux_add_header_example_from_id(conflux_hello examples/hello)
    conflux_add_header_example_from_id(conflux_middleware examples/middleware)
    conflux_add_header_example_from_id(conflux_sse examples/sse)
    conflux_add_header_example_from_id(conflux_static examples/static)
    conflux_add_header_example_from_id(conflux_forms examples/forms)
    conflux_add_header_example_from_id(conflux_gzip examples/gzip)
    conflux_add_header_example_from_id(conflux_http_client examples/http_client)
    conflux_add_header_example_from_id(conflux_dual examples/advanced/dual)
    conflux_add_header_example_from_id(conflux_process_run_example examples/advanced/process_run)
    conflux_add_header_example_from_id(conflux_crypto_sealing_example examples/advanced/crypto_sealing)
    conflux_add_header_example_from_id(conflux_http_observability_example examples/advanced/http_observability)
    conflux_add_header_example_from_id(conflux_http_policy_stack_example examples/advanced/http_policy_stack)
    conflux_add_header_example_from_id(conflux_vhost_openapi_example examples/advanced/vhost_openapi)
    conflux_add_header_example_from_id(conflux_http_client_builder_example examples/advanced/http_client_builder)
    conflux_add_header_example_from_id(conflux_work_join_all_example examples/advanced/work_join_all)
    conflux_add_header_example_from_id(conflux_json_example examples/advanced/json)
    conflux_add_header_example_from_id(conflux_json_config_example examples/advanced/json_config)
    conflux_add_header_example_from_id(conflux_json_stream_ingest_example examples/advanced/json_stream_ingest)
    conflux_add_header_example_from_id(conflux_json_diagnostics_example examples/advanced/json_diagnostics)
    conflux_add_header_example_from_id(conflux_json_transform_example examples/advanced/json_transform)

    if(CONFLUX_HAS_JSON)
        conflux_add_header_example_from_id(conflux_quickstart_json_crud examples/quickstart/json_crud)
        conflux_note_header_example(conflux_quickstart_json_crud built "JSON support enabled")
        conflux_add_header_example_from_id(conflux_api_typed_json_example examples/advanced/manual_json_members)
        conflux_add_header_example_from_id(conflux_http_explicit_offload_example examples/advanced/explicit_offload)
        conflux_add_header_example_from_id(conflux_http_client_json_example examples/advanced/http_client_json)
        conflux_add_header_example_from_id(conflux_custom_json_provider_example examples/advanced/custom_json_provider)
    else()
        conflux_note_header_example(conflux_quickstart_json_crud skipped "JSON HTTP support target unavailable")
    endif()

    if(CONFLUX_WANT_TEMPLATES)
        conflux_add_header_example_from_id(conflux_template_pages_example examples/advanced/template_pages)
    endif()

    if(CONFLUX_HAS_HTTP3)
        conflux_add_header_example_from_id(conflux_h3_probe examples/advanced/h3_probe)
        conflux_add_header_example_from_id(conflux_h3_server examples/advanced/h3_server)
    endif()

    if(CONFLUX_HAS_DB STREQUAL "true")
        conflux_add_header_example_from_id(conflux_quickstart_postgres examples/quickstart/postgres_json)
        conflux_note_header_example(conflux_quickstart_postgres built "DB support enabled and libpq found")
        conflux_add_header_example_from_id(conflux_db_basic examples/advanced/db_basic)
        conflux_note_header_example(conflux_db_basic built "DB support enabled and libpq found")
        conflux_add_header_example_from_id(conflux_db_pool examples/advanced/db_pool)
        conflux_note_header_example(conflux_db_pool built "DB support enabled and libpq found")
        conflux_add_header_example_from_id(conflux_advanced_postgres examples/advanced/postgres)
        conflux_note_header_example(conflux_advanced_postgres built "DB support enabled and libpq found")
    else()
        conflux_note_header_example(conflux_quickstart_postgres skipped "requires CONFLUX_POSTGRES_PROVIDER=LIBPQ/AUTO and libpq")
        conflux_note_header_example(conflux_db_basic skipped "requires CONFLUX_POSTGRES_PROVIDER=LIBPQ/AUTO and libpq")
        conflux_note_header_example(conflux_db_pool skipped "requires CONFLUX_POSTGRES_PROVIDER=LIBPQ/AUTO and libpq")
        conflux_note_header_example(conflux_advanced_postgres skipped "requires CONFLUX_POSTGRES_PROVIDER=LIBPQ/AUTO and libpq")
    endif()

    if(CONFLUX_JSON_REFLECT)
        conflux_add_header_example_from_id(conflux_json_reflect_example examples/advanced/json_reflect)
    endif()

    file(WRITE "${_conflux_examples_manifest}" "Conflux examples manifest\n\n")
    set(_conflux_header_manifest_targets
        conflux_quickstart_hello
        conflux_quickstart_json_crud
        conflux_quickstart_middleware
        conflux_quickstart_openapi
        conflux_quickstart_postgres
        conflux_quickstart_sse
        conflux_quickstart_static_files
        conflux_quickstart_websocket
        conflux_hello
        conflux_middleware
        conflux_sse
        conflux_static
        conflux_forms
        conflux_gzip
        conflux_http_client
        conflux_dual
        conflux_process_run_example
        conflux_crypto_sealing_example
        conflux_http_observability_example
        conflux_http_policy_stack_example
        conflux_vhost_openapi_example
        conflux_http_client_builder_example
        conflux_api_typed_json_example
        conflux_http_client_json_example
        conflux_http_explicit_offload_example
        conflux_work_join_all_example
        conflux_template_pages_example
        conflux_h3_probe
        conflux_h3_server
        conflux_db_basic
        conflux_db_pool
        conflux_advanced_postgres
        conflux_json_example
        conflux_json_config_example
        conflux_json_stream_ingest_example
        conflux_json_diagnostics_example
        conflux_json_transform_example
        conflux_json_reflect_example)
    foreach(_conflux_header_manifest_target IN LISTS _conflux_header_manifest_targets)
        if(TARGET ${_conflux_header_manifest_target})
            conflux_note_header_example(${_conflux_header_manifest_target} built "target available")
        elseif(_conflux_header_manifest_target MATCHES "postgres|db_")
            conflux_note_header_example(${_conflux_header_manifest_target} skipped "requires CONFLUX_POSTGRES_PROVIDER=LIBPQ/AUTO and libpq")
        elseif(_conflux_header_manifest_target MATCHES "json|typed_json|explicit_offload|custom_json")
            conflux_note_header_example(${_conflux_header_manifest_target} skipped "requires JSON support")
        elseif(_conflux_header_manifest_target MATCHES "h3_")
            conflux_note_header_example(${_conflux_header_manifest_target} skipped "requires HTTP/3 support")
        elseif(_conflux_header_manifest_target MATCHES "template")
            conflux_note_header_example(${_conflux_header_manifest_target} skipped "requires template support")
        else()
            conflux_note_header_example(${_conflux_header_manifest_target} skipped "target unavailable")
        endif()
    endforeach()

    add_custom_target(conflux_header_examples)
    add_custom_target(conflux_examples)
    get_property(_conflux_header_example_targets GLOBAL PROPERTY CONFLUX_HEADER_EXAMPLE_TARGETS)
    if(_conflux_header_example_targets)
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
        conflux_add_object_from_id(${_target} "${_source_id}")
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
        http-facade-header/compile-fail-request-view-task
        tests/http_facade_compile_fail_request_view_task
        "Async handlers must take Request const&, not RequestView const&"
        "the view can dangle after coroutine suspension")
    conflux_add_header_compile_fail_test(
        http-facade-header/compile-fail-request-view-task-extractor
        tests/http_facade_compile_fail_request_view_task_extractor
        "Async handlers must take http::Request const&, not http::RequestView const&"
        "the view can dangle after coroutine suspension")
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
        http-facade-header/compile-fail-async-middleware
        tests/http_facade_compile_fail_async_middleware
        "Async middleware must take http::Request const&"
        "RequestView const&")
endfunction()

function(conflux_add_header_benchmark_compile_targets)
    set(_conflux_header_benchmark_source_ids
        benchmarks/bench_common
        benchmarks/crypto_bench
        benchmarks/file_copy_coro_bench
        benchmarks/http_server_bench
        benchmarks/http_server_concurrency_bench
        benchmarks/join_all_N_bench
        benchmarks/json_bench
        benchmarks/router_bench
        benchmarks/socket_raw_bench
        benchmarks/task_cancellation_bench
        benchmarks/task_chain_composition_bench
        benchmarks/task_creation_bench
        benchmarks/tcp_increment_coro_bench
        benchmarks/template_bench
        benchmarks/work_bench
        benchmarks/work_compile_bench
        benchmarks/workpool_enqueue_dequeue_bench)
    if(CONFLUX_HAS_TLS STREQUAL "true")
        list(APPEND _conflux_header_benchmark_source_ids
            benchmarks/tls_tcp_increment_coro_bench)
    endif()
    if(CONFLUX_HAS_DB STREQUAL "true")
        list(APPEND _conflux_header_benchmark_source_ids
            benchmarks/db_coro_bench
            benchmarks/db_params_bench
            benchmarks/db_pipeline_bench)
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
endfunction()
