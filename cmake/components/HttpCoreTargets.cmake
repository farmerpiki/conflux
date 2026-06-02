if(CONFLUX_WANT_HTTP_CORE OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
    conflux_add_module_library(conflux_http_core
        PUBLIC_MODULES
            ${CONFLUX_SRC_ROOT}/net/http_types.cxx
            ${CONFLUX_SRC_ROOT}/net/http_json_string.cxx
            ${CONFLUX_SRC_ROOT}/net/http_request.cxx
            ${CONFLUX_SRC_ROOT}/net/server_types.cxx
    )
    target_link_libraries(conflux_http_core
        PRIVATE conflux_options
        PUBLIC  conflux_crypto
        PUBLIC  conflux_types
        PUBLIC  conflux_utils
    )
endif()

if(CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
    conflux_add_module_library(conflux_http_json
        PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/net/http_json.cxx
    )
    target_link_libraries(conflux_http_json
        PRIVATE conflux_options
        PUBLIC  conflux_http_core
        PUBLIC  conflux_json_boundary
    )
endif()

if(CONFLUX_WANT_HTTP_CORE OR CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_parse_helpers STATIC)
target_sources(conflux_http_parse_helpers
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_parse_helpers.cxx
)
target_link_libraries(conflux_http_parse_helpers
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_utils
)
endif() # CONFLUX_WANT_HTTP_CORE || CONFLUX_WANT_HTTP_SERVER
