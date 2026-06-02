if(TARGET conflux_dns)
    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_address_family
        SOURCE dns_compile_fail_global_address_family.cxx
        TEST dns/compile-fail-global-address-family
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "AddressFamily")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_endpoint
        SOURCE dns_compile_fail_global_endpoint.cxx
        TEST dns/compile-fail-global-endpoint
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "Endpoint")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_resolve_result
        SOURCE dns_compile_fail_global_resolve_result.cxx
        TEST dns/compile-fail-global-resolve-result
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "ResolveResult")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_dns_error
        SOURCE dns_compile_fail_global_dns_error.cxx
        TEST dns/compile-fail-global-dns-error
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "DnsError")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_nameserver_endpoint
        SOURCE dns_compile_fail_global_nameserver_endpoint.cxx
        TEST dns/compile-fail-global-nameserver-endpoint
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "NameserverEndpoint")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_parse_nameserver
        SOURCE dns_compile_fail_global_parse_nameserver.cxx
        TEST dns/compile-fail-global-parse-nameserver
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "parse_nameserver")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_qtype
        SOURCE dns_compile_fail_global_qtype.cxx
        TEST dns/compile-fail-global-qtype
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "QType")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_qclass
        SOURCE dns_compile_fail_global_qclass.cxx
        TEST dns/compile-fail-global-qclass
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "QClass")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_rcode
        SOURCE dns_compile_fail_global_rcode.cxx
        TEST dns/compile-fail-global-rcode
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "RCode")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_header
        SOURCE dns_compile_fail_global_header.cxx
        TEST dns/compile-fail-global-header
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "Header")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_question
        SOURCE dns_compile_fail_global_question.cxx
        TEST dns/compile-fail-global-question
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "Question")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_resource_record
        SOURCE dns_compile_fail_global_resource_record.cxx
        TEST dns/compile-fail-global-resource-record
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "ResourceRecord")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_message
        SOURCE dns_compile_fail_global_message.cxx
        TEST dns/compile-fail-global-message
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "Message")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_edns0_options
        SOURCE dns_compile_fail_global_edns0_options.cxx
        TEST dns/compile-fail-global-edns0-options
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "Edns0Options")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_encode_query
        SOURCE dns_compile_fail_global_encode_query.cxx
        TEST dns/compile-fail-global-encode-query
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "encode_query")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_decode_message
        SOURCE dns_compile_fail_global_decode_message.cxx
        TEST dns/compile-fail-global-decode-message
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "decode_message")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_resolver_backend
        SOURCE dns_compile_fail_global_resolver_backend.cxx
        TEST dns/compile-fail-global-resolver-backend
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "ResolverBackend")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_resolve_options
        SOURCE dns_compile_fail_global_resolve_options.cxx
        TEST dns/compile-fail-global-resolve-options
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "ResolveOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_resolver_options
        SOURCE dns_compile_fail_global_resolver_options.cxx
        TEST dns/compile-fail-global-resolver-options
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "ResolverOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_resolver
        SOURCE dns_compile_fail_global_resolver.cxx
        TEST dns/compile-fail-global-resolver
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "Resolver")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_current_resolver
        SOURCE dns_compile_fail_global_current_resolver.cxx
        TEST dns/compile-fail-global-current-resolver
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "current_resolver")

    conflux_add_compile_fail_test(
        TARGET conflux_dns_compile_fail_global_current_resolver_scope
        SOURCE dns_compile_fail_global_current_resolver_scope.cxx
        TEST dns/compile-fail-global-current-resolver-scope
        LINK conflux_dns conflux_options
        LABELS dns compile-fail
        EXPECT "CurrentResolverScope")
endif()
