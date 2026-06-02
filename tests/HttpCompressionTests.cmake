if(TARGET conflux_http_compression)
    conflux_add_compile_fail_test(
        TARGET conflux_http_compression_compile_fail_global_compress_options
        SOURCE http_compression_compile_fail_global_compress_options.cxx
        TEST http-compression/compile-fail-global-compress-options
        LINK conflux_http_compression conflux_options
        LABELS http compile-fail
        EXPECT "CompressOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_compression_compile_fail_global_gzip_backend
        SOURCE http_compression_compile_fail_global_gzip_backend.cxx
        TEST http-compression/compile-fail-global-gzip-backend
        LINK conflux_http_compression conflux_options
        LABELS http compile-fail
        EXPECT "GzipBackend")

    conflux_add_compile_fail_test(
        TARGET conflux_http_compression_compile_fail_global_compress_middleware
        SOURCE http_compression_compile_fail_global_compress_middleware.cxx
        TEST http-compression/compile-fail-global-compress-middleware
        LINK conflux_http_compression conflux_options
        LABELS http compile-fail
        EXPECT "compress_middleware")
endif()
