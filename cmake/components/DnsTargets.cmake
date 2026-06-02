if(CONFLUX_WANT_DNS)
conflux_add_module_library(conflux_dns
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/net/dns/dns.cxx
    PRIVATE_SOURCES
        ${CONFLUX_SRC_ROOT}/net/dns/dns_impl.cxx
)
target_link_libraries(conflux_dns
    PRIVATE conflux_options
    PUBLIC  conflux_work
    PRIVATE conflux_utils
    PUBLIC  conflux_file_io
    PUBLIC  conflux_socket_io
)

endif() # CONFLUX_WANT_DNS
