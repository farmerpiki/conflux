function(conflux_install_generated_header_file relpath)
    set(_source "${CONFLUX_GENERATED_INCLUDE_DIR}/${relpath}")
    if(NOT EXISTS "${_source}")
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY
        CONFLUX_HEADER_INSTALLED_GENERATED_HEADERS "${relpath}")
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
        file(GLOB_RECURSE _component_headers
            RELATIVE "${CONFLUX_GENERATED_INCLUDE_DIR}"
            "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/${name}/*.hxx")
        set(_install_args
            DIRECTORY "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/${name}/"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/conflux/${name}")
        if(name STREQUAL "json" AND NOT TARGET conflux_json_reflect)
            list(APPEND _install_args
                PATTERN "reflect.hxx" EXCLUDE
                PATTERN "reflect_provider.hxx" EXCLUDE)
            list(FILTER _component_headers EXCLUDE REGEX
                "^conflux/json/(reflect|reflect_provider)\\.hxx$")
        endif()
        install(${_install_args})
        foreach(_component_header IN LISTS _component_headers)
            set_property(GLOBAL APPEND PROPERTY
                CONFLUX_HEADER_INSTALLED_GENERATED_HEADERS
                "${_component_header}")
        endforeach()
    endif()
endfunction()

function(conflux_collect_generated_detail_includes out)
    set(_queue ${ARGN})
    set(_seen)
    set(_detail_headers)
    while(_queue)
        list(POP_FRONT _queue _relpath)
        if(_relpath IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_relpath}")
        set(_source "${CONFLUX_GENERATED_INCLUDE_DIR}/${_relpath}")
        if(NOT EXISTS "${_source}")
            continue()
        endif()
        file(READ "${_source}" _text)
        string(REGEX MATCHALL
            "#[ \t]*include[ \t]*<conflux/detail/generated/[^>]+>"
            _matches "${_text}")
        foreach(_match IN LISTS _matches)
            string(REGEX REPLACE ".*<([^>]+)>.*" "\\1" _detail_header "${_match}")
            if(NOT _detail_header IN_LIST _detail_headers)
                list(APPEND _detail_headers "${_detail_header}")
                list(APPEND _queue "${_detail_header}")
            endif()
        endforeach()
    endwhile()
    set(${out} "${_detail_headers}" PARENT_SCOPE)
endfunction()

function(conflux_install_generated_detail_headers)
    get_property(_installed_headers GLOBAL PROPERTY
        CONFLUX_HEADER_INSTALLED_GENERATED_HEADERS)
    if(NOT _installed_headers)
        return()
    endif()
    list(REMOVE_DUPLICATES _installed_headers)
    conflux_collect_generated_detail_includes(_detail_headers
        ${_installed_headers})
    foreach(_detail_header IN LISTS _detail_headers)
        conflux_install_generated_header_file("${_detail_header}")
    endforeach()
endfunction()

function(conflux_install_registered_public_headers)
    conflux_install_generated_header_file("conflux.hxx")
    conflux_install_generated_header_file("conflux/config.hxx")
    conflux_install_generated_header_file("conflux/features.hxx")

    foreach(_component IN LISTS CONFLUX_PACKAGE_COMPONENTS)
        conflux_install_generated_header_component("${_component}")
    endforeach()

    if(CONFLUX_HEADER_INSTALL_RUNTIME_COMPONENTS)
        foreach(_support_header IN ITEMS
                conflux/crypto.hxx
                conflux/file_io.hxx
                conflux/file_io/buffers.hxx
                conflux/file_io/driver.hxx
                conflux/file_io/iopoll.hxx
                conflux/file_io/pipe_pool.hxx
                conflux/file_io/reader.hxx
                conflux/file_map/types.hxx
                conflux/net/app.hxx
                conflux/net/app/defer.hxx
                conflux/net/app/extractor_helpers.hxx
                conflux/net/app/json_helpers.hxx
                conflux/net/app/metadata_helpers.hxx
                conflux/net/app/openapi.hxx
                conflux/net/app/response.hxx
                conflux/net/app/route_helpers.hxx
                conflux/net/app/traits.hxx
                conflux/net/app/types.hxx
                conflux/net/auth.hxx
                conflux/net/cancel.hxx
                conflux/net/config.hxx
                conflux/net/http/app_json.hxx
                conflux/net/http/json.hxx
                conflux/net/http/json_string.hxx
                conflux/net/http/native_json.hxx
                conflux/net/http/parse_helpers.hxx
                conflux/net/http/realtime.hxx
                conflux/net/http/request.hxx
                conflux/net/http/response.hxx
                conflux/net/http/response_json.hxx
                conflux/net/http/server_types.hxx
                conflux/net/http/static_files.hxx
                conflux/net/http/types.hxx
                conflux/net/http_server.hxx
                conflux/net/metrics.hxx
                conflux/net/observability.hxx
                conflux/net/path.hxx
                conflux/net/rate_limit.hxx
                conflux/net/request_id.hxx
                conflux/net/router.hxx
                conflux/net/router_match.hxx
                conflux/net/security.hxx
                conflux/net/tls.hxx
                conflux/net/tracing.hxx
                conflux/net/vhost.hxx
                conflux/small_function.hxx
                conflux/socket_io.hxx
                conflux/socket_io/coro.hxx
                conflux/uring.hxx
                conflux/uring/completion.hxx
                conflux/uring/fd.hxx
                conflux/uring/handle.hxx
                conflux/uring/sqe.hxx
                conflux/uring/timeout.hxx
                conflux/utils.hxx)
            conflux_install_generated_header_file("${_support_header}")
        endforeach()
    endif()

    get_property(_hpp_names GLOBAL PROPERTY CONFLUX_HEADER_PUBLIC_HPP_NAMES)
    foreach(_hpp_name IN LISTS _hpp_names)
        set(_hpp_source "${CONFLUX_PUBLIC_HPP_DIR}/${_hpp_name}.hpp")
        if(EXISTS "${_hpp_source}")
            install(FILES "${_hpp_source}"
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/conflux")
        endif()
        conflux_install_generated_header_file("conflux/${_hpp_name}.hxx")
    endforeach()

    set(_conflux_hpp "${CONFLUX_PUBLIC_HPP_DIR}/conflux.hpp")
    if(EXISTS "${_conflux_hpp}")
        install(FILES "${_conflux_hpp}"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/conflux")
    endif()

    conflux_install_generated_detail_headers()
endfunction()
