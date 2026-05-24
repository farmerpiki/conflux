if(NOT _conflux_simd_backend STREQUAL "OFF" AND _conflux_simd_selection STREQUAL "DIRECT")
    set(_conflux_simd_direct_shape_objects)
    set(_conflux_simd_direct_shape_deps)
    if(TARGET conflux_json)
        list(APPEND _conflux_simd_direct_shape_objects
            json_api.cxx.o
            json_dump.cxx.o)
        list(APPEND _conflux_simd_direct_shape_deps conflux_json)
    endif()
    if(TARGET conflux_utils)
        list(APPEND _conflux_simd_direct_shape_objects utils.cxx.o)
        list(APPEND _conflux_simd_direct_shape_deps conflux_utils)
    endif()
    if(TARGET conflux_crypto)
        list(APPEND _conflux_simd_direct_shape_objects crypto.cxx.o)
        list(APPEND _conflux_simd_direct_shape_deps conflux_crypto)
    endif()
    if(TARGET conflux_http_realtime)
        list(APPEND _conflux_simd_direct_shape_objects realtime.cxx.o)
        list(APPEND _conflux_simd_direct_shape_deps conflux_http_realtime)
    endif()
    if(_conflux_simd_direct_shape_objects)
        add_custom_target(conflux_simd_direct_shape
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-simd-direct-shape.py"
                    "${CMAKE_BINARY_DIR}"
                    "${CMAKE_NM}"
                    ${_conflux_simd_direct_shape_objects}
            DEPENDS ${_conflux_simd_direct_shape_deps}
            VERBATIM)
    endif()
    unset(_conflux_simd_direct_shape_deps)
    unset(_conflux_simd_direct_shape_objects)
endif()
