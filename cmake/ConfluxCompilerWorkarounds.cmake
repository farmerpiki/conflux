function(conflux_apply_template_compiler_workarounds source)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC 15 currently ICEs in the ealias pass on this C++ module
        # implementation unit at release optimization levels. Function-level
        # optimize attributes are ignored for modules, so keep the workaround
        # scoped to this source file.
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-O0")
    endif()
endfunction()

function(conflux_apply_http_server_compiler_workarounds source)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GNU release builds have needed this fallback for the HTTP send
        # partition. Keep it scoped to this source file instead of disabling
        # optimization or LTO for the wider target.
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-O0")
    endif()
endfunction()
