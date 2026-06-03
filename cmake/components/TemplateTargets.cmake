if(CONFLUX_WANT_TEMPLATES)
conflux_add_module_library(conflux_template
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/template.cxx
    PRIVATE_SOURCES
        ${CONFLUX_SRC_ROOT}/template_env.cxx
        ${CONFLUX_SRC_ROOT}/template_eval.cxx
        ${CONFLUX_SRC_ROOT}/template_filters.cxx
        ${CONFLUX_SRC_ROOT}/template_impl.cxx
        ${CONFLUX_SRC_ROOT}/template_parse.cxx
        ${CONFLUX_SRC_ROOT}/template_render.cxx
        ${CONFLUX_SRC_ROOT}/template_value_ops.cxx
)
conflux_apply_template_compiler_workarounds("${CONFLUX_SRC_ROOT}/template_impl.cxx")
target_link_libraries(conflux_template
    PRIVATE conflux_options
    PUBLIC  conflux_json
    PUBLIC  conflux_utils
    PUBLIC  conflux_file_io_sync
)
endif() # CONFLUX_WANT_TEMPLATES

if(CONFLUX_WANT_TEMPLATES_WATCH)
conflux_add_module_library(conflux_template_watch
    PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/template_watch.cxx
)
target_link_libraries(conflux_template_watch
    PRIVATE conflux_options
    PUBLIC  conflux_template
    PUBLIC  conflux_file_watch
)
endif() # CONFLUX_WANT_TEMPLATES_WATCH
