include(Dependencies)
string(TOUPPER "${CONFLUX_TLS_PROVIDER}" CONFLUX_TLS_PROVIDER_UPPER)
if((CONFLUX_WANT_HTTP_SERVER OR CONFLUX_WANT_HTTP_CLIENT OR CONFLUX_WANT_HTTP_AUTH)
        AND NOT CONFLUX_TLS_PROVIDER_UPPER STREQUAL "OFF")
    find_package(OpenSSL)
endif()
if(CONFLUX_WANT_HTTP_COMPRESSION)
    find_package(ZLIB)
endif()
string(TOUPPER "${CONFLUX_HTTP2_PROVIDER}" CONFLUX_HTTP2_PROVIDER_UPPER)
string(TOUPPER "${CONFLUX_HTTP3_PROVIDER}" CONFLUX_HTTP3_PROVIDER_UPPER)
string(TOUPPER "${CONFLUX_GZIP_PROVIDER}" CONFLUX_GZIP_PROVIDER_UPPER)
string(TOUPPER "${CONFLUX_BROTLI_PROVIDER}" CONFLUX_BROTLI_PROVIDER_UPPER)
string(TOUPPER "${CONFLUX_ZSTD_PROVIDER}" CONFLUX_ZSTD_PROVIDER_UPPER)
string(TOUPPER "${CONFLUX_POSTGRES_PROVIDER}" CONFLUX_POSTGRES_PROVIDER_UPPER)
string(TOUPPER "${CONFLUX_PASSWORD_HASH_ARGON2_PROVIDER}" CONFLUX_ARGON2_PROVIDER_UPPER)

set(_conflux_header_wants_tls FALSE)
if(CONFLUX_WANT_HTTP_SERVER OR CONFLUX_WANT_HTTP_CLIENT OR CONFLUX_WANT_HTTP_AUTH)
    set(_conflux_header_wants_tls TRUE)
endif()
set(CONFLUX_HAS_TLS "false")
set(CONFLUX_RESOLVED_TLS_PROVIDER "OFF" CACHE STRING "Resolved TLS provider" FORCE)
if(_conflux_header_wants_tls AND NOT CONFLUX_TLS_PROVIDER_UPPER STREQUAL "OFF")
    if(OpenSSL_FOUND)
        set(CONFLUX_HAS_TLS "true")
        set(CONFLUX_RESOLVED_TLS_PROVIDER "OPENSSL" CACHE STRING "Resolved TLS provider" FORCE)
    elseif(CONFLUX_TLS_PROVIDER_UPPER STREQUAL "OPENSSL")
        message(FATAL_ERROR "conflux: CONFLUX_TLS_PROVIDER=OPENSSL requires OpenSSL")
    endif()
endif()

set(CONFLUX_HAS_HTTP2 "false")
set(CONFLUX_RESOLVED_HTTP2_PROVIDER "OFF" CACHE STRING "Resolved HTTP/2 provider" FORCE)
if((CONFLUX_WANT_HTTP_SERVER OR CONFLUX_WANT_HTTP_CLIENT)
        AND NOT CONFLUX_HTTP2_PROVIDER_UPPER STREQUAL "OFF")
    if(NGHTTP2_FOUND AND CONFLUX_HAS_TLS STREQUAL "true")
        set(CONFLUX_HAS_HTTP2 "true")
        set(CONFLUX_RESOLVED_HTTP2_PROVIDER "NGHTTP2" CACHE STRING "Resolved HTTP/2 provider" FORCE)
    elseif(CONFLUX_HTTP2_PROVIDER_UPPER STREQUAL "NGHTTP2")
        message(FATAL_ERROR "conflux: CONFLUX_HTTP2_PROVIDER=NGHTTP2 requires libnghttp2 and TLS")
    endif()
endif()

if(CONFLUX_WANT_HTTP_SERVER
        AND CONFLUX_HTTP3_PROVIDER_UPPER STREQUAL "NGTCP2_NGHTTP3_OPENSSL"
        AND NOT CONFLUX_ENABLE_EXPERIMENTAL)
    message(FATAL_ERROR
        "conflux: explicit HTTP/3 provider requires CONFLUX_ENABLE_EXPERIMENTAL=ON")
endif()
set(CONFLUX_HAS_HTTP3 "false")
set(CONFLUX_RESOLVED_HTTP3_PROVIDER "OFF" CACHE STRING "Resolved HTTP/3 provider" FORCE)
if(CONFLUX_WANT_HTTP_SERVER
        AND CONFLUX_ENABLE_EXPERIMENTAL
        AND NOT CONFLUX_HTTP3_PROVIDER_UPPER STREQUAL "OFF")
    if(OpenSSL_FOUND AND OPENSSL_VERSION VERSION_GREATER_EQUAL "3.5.0"
            AND CONFLUX_HAS_TLS STREQUAL "true"
            AND NGTCP2_FOUND
            AND NGTCP2_CRYPTO_OSSL_FOUND
            AND NGHTTP3_FOUND)
        set(CONFLUX_HAS_HTTP3 "true")
        set(CONFLUX_RESOLVED_HTTP3_PROVIDER "NGTCP2_NGHTTP3_OPENSSL" CACHE STRING "Resolved HTTP/3 provider" FORCE)
    elseif(CONFLUX_HTTP3_PROVIDER_UPPER STREQUAL "NGTCP2_NGHTTP3_OPENSSL")
        message(FATAL_ERROR
            "conflux: CONFLUX_HTTP3_PROVIDER=NGTCP2_NGHTTP3_OPENSSL requires ngtcp2, ngtcp2_crypto_ossl, nghttp3, TLS, and OpenSSL >= 3.5")
    endif()
endif()

if(CONFLUX_WANT_JSON)
    set(CONFLUX_HAS_JSON "true")
else()
    set(CONFLUX_HAS_JSON "false")
endif()

set(CONFLUX_HAS_COMPRESS "false")
set(CONFLUX_HAS_ZLIB "false")
set(CONFLUX_HAS_LIBDEFLATE "false")
set(CONFLUX_HAS_ZLIB_NG "false")
set(CONFLUX_HAS_ISAL "false")
set(CONFLUX_RESOLVED_GZIP_PROVIDER "OFF" CACHE STRING
    "Resolved gzip backend for this build" FORCE)
if(CONFLUX_WANT_HTTP_COMPRESSION AND NOT CONFLUX_GZIP_PROVIDER_UPPER STREQUAL "OFF")
    if(CONFLUX_GZIP_PROVIDER_UPPER STREQUAL "ALL")
        if(LIBDEFLATE_FOUND)
            set(CONFLUX_HAS_LIBDEFLATE "true")
        endif()
        if(ZLIB_NG_FOUND)
            set(CONFLUX_HAS_ZLIB_NG "true")
        endif()
        if(ZLIB_FOUND)
            set(CONFLUX_HAS_ZLIB "true")
        endif()
        if(LIBISAL_FOUND)
            set(CONFLUX_HAS_ISAL "true")
        endif()
        set(CONFLUX_RESOLVED_GZIP_PROVIDER "ALL" CACHE STRING
            "Resolved gzip backend for this build" FORCE)
    elseif(CONFLUX_GZIP_PROVIDER_UPPER STREQUAL "AUTO")
        if(LIBDEFLATE_FOUND)
            set(CONFLUX_HAS_LIBDEFLATE "true")
            set(CONFLUX_RESOLVED_GZIP_PROVIDER "LIBDEFLATE" CACHE STRING
                "Resolved gzip backend for this build" FORCE)
        elseif(ZLIB_NG_FOUND)
            set(CONFLUX_HAS_ZLIB_NG "true")
            set(CONFLUX_RESOLVED_GZIP_PROVIDER "ZLIB_NG" CACHE STRING
                "Resolved gzip backend for this build" FORCE)
        elseif(ZLIB_FOUND)
            set(CONFLUX_HAS_ZLIB "true")
            set(CONFLUX_RESOLVED_GZIP_PROVIDER "ZLIB" CACHE STRING
                "Resolved gzip backend for this build" FORCE)
        endif()
    elseif(CONFLUX_GZIP_PROVIDER_UPPER STREQUAL "LIBDEFLATE" AND LIBDEFLATE_FOUND)
        set(CONFLUX_HAS_LIBDEFLATE "true")
        set(CONFLUX_RESOLVED_GZIP_PROVIDER "LIBDEFLATE" CACHE STRING
            "Resolved gzip backend for this build" FORCE)
    elseif(CONFLUX_GZIP_PROVIDER_UPPER STREQUAL "ZLIB_NG" AND ZLIB_NG_FOUND)
        set(CONFLUX_HAS_ZLIB_NG "true")
        set(CONFLUX_RESOLVED_GZIP_PROVIDER "ZLIB_NG" CACHE STRING
            "Resolved gzip backend for this build" FORCE)
    elseif(CONFLUX_GZIP_PROVIDER_UPPER STREQUAL "ZLIB" AND ZLIB_FOUND)
        set(CONFLUX_HAS_ZLIB "true")
        set(CONFLUX_RESOLVED_GZIP_PROVIDER "ZLIB" CACHE STRING
            "Resolved gzip backend for this build" FORCE)
    elseif(CONFLUX_GZIP_PROVIDER_UPPER STREQUAL "ISAL" AND LIBISAL_FOUND)
        set(CONFLUX_HAS_ISAL "true")
        set(CONFLUX_RESOLVED_GZIP_PROVIDER "ISAL" CACHE STRING
            "Resolved gzip backend for this build" FORCE)
    else()
        message(FATAL_ERROR
            "conflux: CONFLUX_GZIP_PROVIDER=${CONFLUX_GZIP_PROVIDER_UPPER} was requested but its provider was not found")
    endif()
    if(CONFLUX_HAS_ZLIB STREQUAL "true"
            OR CONFLUX_HAS_LIBDEFLATE STREQUAL "true"
            OR CONFLUX_HAS_ZLIB_NG STREQUAL "true"
            OR CONFLUX_HAS_ISAL STREQUAL "true")
        set(CONFLUX_HAS_COMPRESS "true")
    endif()
endif()

set(CONFLUX_HAS_BROTLI "false")
set(CONFLUX_RESOLVED_BROTLI_PROVIDER "OFF" CACHE STRING "Resolved Brotli provider" FORCE)
if(CONFLUX_WANT_HTTP_COMPRESSION AND NOT CONFLUX_BROTLI_PROVIDER_UPPER STREQUAL "OFF")
    if(BROTLI_FOUND)
        set(CONFLUX_HAS_BROTLI "true")
        set(CONFLUX_RESOLVED_BROTLI_PROVIDER "SYSTEM" CACHE STRING "Resolved Brotli provider" FORCE)
    elseif(CONFLUX_BROTLI_PROVIDER_UPPER STREQUAL "SYSTEM")
        message(FATAL_ERROR "conflux: CONFLUX_BROTLI_PROVIDER=SYSTEM requires Brotli pkg-config packages")
    endif()
endif()

set(CONFLUX_HAS_ZSTD "false")
set(CONFLUX_RESOLVED_ZSTD_PROVIDER "OFF" CACHE STRING "Resolved zstd provider" FORCE)
if(CONFLUX_WANT_HTTP_COMPRESSION AND NOT CONFLUX_ZSTD_PROVIDER_UPPER STREQUAL "OFF")
    if(ZSTD_FOUND)
        set(CONFLUX_HAS_ZSTD "true")
        set(CONFLUX_RESOLVED_ZSTD_PROVIDER "SYSTEM" CACHE STRING "Resolved zstd provider" FORCE)
    elseif(CONFLUX_ZSTD_PROVIDER_UPPER STREQUAL "SYSTEM")
        message(FATAL_ERROR "conflux: CONFLUX_ZSTD_PROVIDER=SYSTEM requires libzstd")
    endif()
endif()

if(CONFLUX_WANT_FILE_WATCH)
    set(CONFLUX_HAS_FILE_WATCH "true")
else()
    set(CONFLUX_HAS_FILE_WATCH "false")
endif()
if(CONFLUX_WANT_TEMPLATES)
    set(CONFLUX_HAS_TEMPLATES "true")
else()
    set(CONFLUX_HAS_TEMPLATES "false")
endif()
if(CONFLUX_WANT_TEMPLATES_WATCH)
    set(CONFLUX_HAS_TEMPLATES_WATCH "true")
else()
    set(CONFLUX_HAS_TEMPLATES_WATCH "false")
endif()
if(CONFLUX_ENABLE_METRICS)
    set(CONFLUX_HAS_METRICS "true")
else()
    set(CONFLUX_HAS_METRICS "false")
endif()

set(CONFLUX_HAS_DB "false")
set(CONFLUX_RESOLVED_POSTGRES_PROVIDER "OFF" CACHE STRING "Resolved PostgreSQL provider" FORCE)
if(CONFLUX_WANT_DB_POSTGRES AND NOT CONFLUX_POSTGRES_PROVIDER_UPPER STREQUAL "OFF")
    if(LIBPQ_FOUND)
        set(CONFLUX_HAS_DB "true")
        set(CONFLUX_RESOLVED_POSTGRES_PROVIDER "LIBPQ" CACHE STRING "Resolved PostgreSQL provider" FORCE)
    elseif(CONFLUX_POSTGRES_PROVIDER_UPPER STREQUAL "LIBPQ")
        message(FATAL_ERROR "conflux: CONFLUX_POSTGRES_PROVIDER=LIBPQ requires libpq")
    endif()
endif()


set(CONFLUX_RESOLVED_ARGON2_PROVIDER "OFF" CACHE STRING
    "Resolved Argon2 password-hashing provider" FORCE)
if((CONFLUX_WANT_HTTP_AUTH OR CONFLUX_WANT_HTTP_SERVER)
        AND NOT CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "OFF")
    if(CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "SYSTEM" AND NOT ARGON2_FOUND)
        message(FATAL_ERROR "conflux: Argon2 provider SYSTEM requires pkg-config module libargon2")
    elseif((CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "AUTO"
            OR CONFLUX_ARGON2_PROVIDER_UPPER STREQUAL "SYSTEM")
            AND ARGON2_FOUND)
        set(CONFLUX_RESOLVED_ARGON2_PROVIDER "SYSTEM" CACHE STRING
            "Resolved Argon2 password-hashing provider" FORCE)
    else()
        set(CONFLUX_RESOLVED_ARGON2_PROVIDER "RUNTIME" CACHE STRING
            "Resolved Argon2 password-hashing provider" FORCE)
    endif()
endif()

	foreach(_feature IN ITEMS
			TLS COMPRESS ZLIB LIBDEFLATE ZLIB_NG ISAL BROTLI ZSTD HTTP2 HTTP3)
		if(NOT DEFINED CONFLUX_HAS_${_feature})
			set(CONFLUX_HAS_${_feature} "false")
		endif()
	endforeach()
	if(NOT DEFINED CONFLUX_HAS_JSON)
		if(CONFLUX_WANT_JSON)
			set(CONFLUX_HAS_JSON "true")
		else()
			set(CONFLUX_HAS_JSON "false")
		endif()
	endif()
	if(NOT DEFINED CONFLUX_HAS_FILE_WATCH)
		if(CONFLUX_WANT_FILE_WATCH)
			set(CONFLUX_HAS_FILE_WATCH "true")
		else()
			set(CONFLUX_HAS_FILE_WATCH "false")
		endif()
	endif()
	if(NOT DEFINED CONFLUX_HAS_TEMPLATES)
		if(CONFLUX_WANT_TEMPLATES)
			set(CONFLUX_HAS_TEMPLATES "true")
		else()
			set(CONFLUX_HAS_TEMPLATES "false")
		endif()
	endif()
	if(NOT DEFINED CONFLUX_HAS_TEMPLATES_WATCH)
		if(CONFLUX_WANT_TEMPLATES_WATCH)
			set(CONFLUX_HAS_TEMPLATES_WATCH "true")
		else()
			set(CONFLUX_HAS_TEMPLATES_WATCH "false")
		endif()
	endif()
	if(NOT DEFINED CONFLUX_HAS_METRICS)
		if(CONFLUX_ENABLE_METRICS)
			set(CONFLUX_HAS_METRICS "true")
		else()
			set(CONFLUX_HAS_METRICS "false")
		endif()
	endif()
	if(NOT DEFINED CONFLUX_HAS_DB)
		string(TOUPPER "${CONFLUX_POSTGRES_PROVIDER}" CONFLUX_POSTGRES_PROVIDER_UPPER)
    if(LIBPQ_FOUND AND NOT CONFLUX_POSTGRES_PROVIDER_UPPER STREQUAL "OFF")
        set(CONFLUX_HAS_DB "true")
    else()
        set(CONFLUX_HAS_DB "false")
    endif()
endif()
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

conflux_header_public_component(conflux_core core
    IMPLS conflux_header_impl_core
    COMPILE_DEFINITIONS CONFLUX_INTERFACE_HEADER=1)

conflux_header_public_component(conflux_types types
    IMPLS conflux_header_impl_core)

set(CONFLUX_PACKAGE_SUPPORT_COMPONENTS)
set(CONFLUX_PACKAGE_SUPPORT_TARGETS)
set(CONFLUX_PACKAGE_ALL_COMPONENTS ${CONFLUX_PACKAGE_COMPONENTS})
set(CONFLUX_PACKAGE_ALL_TARGETS ${CONFLUX_PACKAGE_TARGETS})
set(CONFLUX_PACKAGE_SUPPORT_COMPONENTS headers)
set(CONFLUX_PACKAGE_SUPPORT_TARGETS conflux::headers)
list(PREPEND CONFLUX_PACKAGE_ALL_COMPONENTS headers)
list(PREPEND CONFLUX_PACKAGE_ALL_TARGETS conflux::headers)
list(PREPEND CONFLUX_HEADER_INSTALL_TARGETS conflux_headers)
get_property(_conflux_header_impl_targets GLOBAL PROPERTY CONFLUX_HEADER_IMPL_TARGETS)
get_property(_conflux_header_impl_components GLOBAL PROPERTY CONFLUX_HEADER_IMPL_COMPONENTS)
get_property(_conflux_header_impl_namespaced_targets GLOBAL PROPERTY CONFLUX_HEADER_IMPL_NAMESPACED_TARGETS)
if(TARGET conflux_header_impl)
    list(APPEND CONFLUX_HEADER_INSTALL_TARGETS conflux_header_impl)
    list(APPEND CONFLUX_PACKAGE_SUPPORT_COMPONENTS header_impl)
    list(APPEND CONFLUX_PACKAGE_SUPPORT_TARGETS conflux::header_impl)
    list(APPEND CONFLUX_PACKAGE_ALL_COMPONENTS header_impl)
    list(APPEND CONFLUX_PACKAGE_ALL_TARGETS conflux::header_impl)
endif()
if(_conflux_header_impl_targets)
    list(APPEND CONFLUX_HEADER_INSTALL_TARGETS ${_conflux_header_impl_targets})
    list(APPEND CONFLUX_PACKAGE_SUPPORT_COMPONENTS ${_conflux_header_impl_components})
    list(APPEND CONFLUX_PACKAGE_SUPPORT_TARGETS ${_conflux_header_impl_namespaced_targets})
    list(APPEND CONFLUX_PACKAGE_ALL_COMPONENTS ${_conflux_header_impl_components})
    list(APPEND CONFLUX_PACKAGE_ALL_TARGETS ${_conflux_header_impl_namespaced_targets})
endif()
set(CONFLUX_INSTALL_MOCK_RUNTIME FALSE)
if(CONFLUX_USE_MOCK_LIBURING)
    message(STATUS
        "conflux: mock liburing is enabled; runtime/http installed package components are disabled because they are compile-only in this build.")
endif()
set(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS FALSE)
if(NOT CONFLUX_USE_MOCK_LIBURING AND TARGET PkgConfig::LIBURING)
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
    conflux_header_public_component(conflux_json json
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
    conflux_header_public_component(conflux_net_http http
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

conflux_header_public_component(conflux_file_io_sync file_io_sync
    IMPLS conflux_header_impl_file_io_sync)

if(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS)
    conflux_header_public_component(conflux_work work
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
    set(_conflux_db_links conflux_work)
    if(TARGET PkgConfig::LIBPQ)
        list(APPEND _conflux_db_links PkgConfig::LIBPQ)
    endif()
    conflux_header_public_component(conflux_db db
        IMPLS conflux_header_impl_db
        LINKS ${_conflux_db_links})

    conflux_header_public_component(conflux_pg pg
        LINKS conflux_db)
    unset(_conflux_db_links)
endif()

set(CONFLUX_PUBLIC_HPP_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/public-hpp/conflux")
conflux_register_header_public_hpp(config)
if(NOT CONFLUX_USE_MOCK_LIBURING)
    conflux_register_header_public_hpp(curated)
    conflux_register_header_public_hpp(extended)
    conflux_register_header_public_hpp(complete)
endif()
conflux_write_header_public_hpp_files("${CONFLUX_PUBLIC_HPP_DIR}")

conflux_add_header_component_smoke_targets()
conflux_add_header_link_smoke_targets()
conflux_add_header_examples_from_source_ids()
conflux_add_header_test_compile_targets()
conflux_add_header_compile_fail_tests()
conflux_add_header_benchmark_compile_targets()

function(conflux_install_generated_header_file relpath)
    set(_source "${CONFLUX_GENERATED_INCLUDE_DIR}/${relpath}")
    if(NOT EXISTS "${_source}")
        return()
    endif()
    get_filename_component(_dest_dir "${relpath}" DIRECTORY)
    if(_dest_dir)
        install(FILES "${_source}"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${_dest_dir}")
    else()
        install(FILES "${_source}"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    endif()
endfunction()

function(conflux_install_generated_header_component name)
    conflux_install_generated_header_file("conflux/${name}.hxx")
    if(EXISTS "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/${name}")
        set(_install_args
            DIRECTORY "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/${name}/"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/conflux/${name}")
        if(name STREQUAL "json" AND NOT TARGET conflux_json_reflect)
            list(APPEND _install_args
                PATTERN "reflect.hxx" EXCLUDE
                PATTERN "reflect_provider.hxx" EXCLUDE)
        endif()
        install(${_install_args})
    endif()
endfunction()

function(conflux_install_registered_public_headers)
    if(NOT CONFLUX_USE_MOCK_LIBURING)
        conflux_install_generated_header_file("conflux.hxx")
    endif()
    conflux_install_generated_header_file("conflux/config.hxx")
    conflux_install_generated_header_file("conflux/features.hxx")

    foreach(_component IN LISTS CONFLUX_PACKAGE_COMPONENTS)
        conflux_install_generated_header_component("${_component}")
    endforeach()

    get_property(_hpp_names GLOBAL PROPERTY CONFLUX_HEADER_PUBLIC_HPP_NAMES)
    foreach(_hpp_name IN LISTS _hpp_names)
        set(_hpp_source "${CONFLUX_PUBLIC_HPP_DIR}/${_hpp_name}.hpp")
        if(EXISTS "${_hpp_source}")
            install(FILES "${_hpp_source}"
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/conflux")
        endif()
        conflux_install_generated_header_file("conflux/${_hpp_name}.hxx")
    endforeach()

    if(NOT CONFLUX_USE_MOCK_LIBURING)
        set(_conflux_hpp "${CONFLUX_PUBLIC_HPP_DIR}/conflux.hpp")
        if(EXISTS "${_conflux_hpp}")
            install(FILES "${_conflux_hpp}"
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/conflux")
        endif()
    endif()

    install(DIRECTORY "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/detail/generated/"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/detail/generated)
endfunction()

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
    set(_conflux_package_smoke_args
        --source "${CMAKE_CURRENT_SOURCE_DIR}"
        --prefix "${CONFLUX_PACKAGE_SMOKE_PREFIX}"
        --build-dir "${CMAKE_CURRENT_BINARY_DIR}/package-smoke"
        --components "${CONFLUX_PACKAGE_SMOKE_COMPONENTS}")
    if(CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE)
        list(APPEND _conflux_package_smoke_args
            --interface-mode "${CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE}")
    endif()
    if(CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER)
        list(APPEND _conflux_package_smoke_args --mixed-module-header)
    endif()
    add_test(NAME build/package-config-install-tree
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-package-config-smoke.sh"
                ${_conflux_package_smoke_args})
    set_tests_properties(build/package-config-install-tree PROPERTIES
        LABELS "build;package;install")
endif()

if((CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
        AND CONFLUX_RUN_INSTALL_TREE_SMOKE)
    add_test(NAME build/install-tree-smoke
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-install-tree-smoke.sh"
                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                --build-dir "${CONFLUX_INSTALL_TREE_SMOKE_BUILD_DIR}"
                --prefix "${CONFLUX_INSTALL_TREE_SMOKE_PREFIX}"
                --smoke-build-dir "${CONFLUX_INSTALL_TREE_SMOKE_CONSUMER_BUILD_DIR}"
                --feature-set "${CONFLUX_INSTALL_TREE_SMOKE_FEATURE_SET}"
                --build-type "${CONFLUX_INSTALL_TREE_SMOKE_BUILD_TYPE}"
                --generator "${CONFLUX_INSTALL_TREE_SMOKE_GENERATOR}"
                --interface-mode "${CONFLUX_INSTALL_TREE_SMOKE_INTERFACE_MODE}"
                --components "${CONFLUX_PACKAGE_SMOKE_COMPONENTS}"
                $<$<BOOL:${CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER}>:--mixed-module-header-smoke>
                -- ${CONFLUX_INSTALL_TREE_SMOKE_EXTRA_CMAKE_ARGS})
    set_tests_properties(build/install-tree-smoke PROPERTIES
        LABELS "build;package;install")
endif()
