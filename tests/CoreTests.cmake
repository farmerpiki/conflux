add_executable(conflux_crypto_tests crypto_test.cxx)
target_link_libraries(conflux_crypto_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

if(_conflux_cpu_feature_probes_runtime AND CONFLUX_HAS_AESNI)
    add_test(NAME simd/crypto-aesni-runtime-scalar-fallback
        COMMAND conflux_crypto_tests "crypto: aes_gcm empty plaintext decrypt")
    set_tests_properties(simd/crypto-aesni-runtime-scalar-fallback PROPERTIES
        ENVIRONMENT "CONFLUX_TEST_CPU_FEATURES_DISABLE=aesni_pclmul_sse41"
        LABELS "simd;crypto"
        RUN_SERIAL TRUE)
endif()

set(CONFLUX_BUILD_AES_GCM_COMPARE TRUE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC modules conflict/ICE when this standalone OpenSSL comparison TU includes
    # OpenSSL headers; the Catch2 crypto coverage above still builds and runs.
    set(CONFLUX_BUILD_AES_GCM_COMPARE FALSE)
endif()
if(CONFLUX_HAS_TLS STREQUAL "true" AND CONFLUX_BUILD_AES_GCM_COMPARE)
    add_executable(conflux_aes_gcm_compare aes_gcm_openssl_compare.cxx)
    target_link_libraries(conflux_aes_gcm_compare PRIVATE conflux conflux_options OpenSSL::SSL OpenSSL::Crypto)
endif()

add_executable(conflux_utils_tests utils_test.cxx)
target_link_libraries(conflux_utils_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

if(_conflux_cpu_feature_probes_runtime AND CONFLUX_HAS_AESNI)
    add_test(NAME simd/hex-ssse3-runtime-scalar-fallback
        COMMAND conflux_utils_tests "utils: hex_encode known values")
    set_tests_properties(simd/hex-ssse3-runtime-scalar-fallback PROPERTIES
        ENVIRONMENT "CONFLUX_TEST_CPU_FEATURES_DISABLE=aesni_pclmul_sse41"
        LABELS "simd;crypto"
        RUN_SERIAL TRUE)
endif()

if(CONFLUX_HAS_TLS STREQUAL "true")
    add_executable(conflux_jwt_tests jwt_test.cxx)
    target_link_libraries(conflux_jwt_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)
endif()

add_executable(conflux_password_hash_tests password_hash_test.cxx)
target_link_libraries(conflux_password_hash_tests PRIVATE conflux_http_auth conflux_options Catch2::Catch2WithMain)

if(CONFLUX_HAS_TLS STREQUAL "true")
    add_executable(conflux_auth_secret_config_tests auth_secret_config_test.cxx)
    target_link_libraries(conflux_auth_secret_config_tests PRIVATE conflux_http_auth conflux_net_config conflux_options Catch2::Catch2WithMain)
endif()

add_executable(conflux_auth_rate_limit_tests auth_rate_limit_test.cxx)
target_link_libraries(conflux_auth_rate_limit_tests PRIVATE conflux_http_auth conflux_options Catch2::Catch2WithMain)
if(TARGET conflux_http_server_config)
    add_executable(conflux_config_tests config_test.cxx)
    target_link_libraries(conflux_config_tests
        PRIVATE
            conflux_net_config
            conflux_uring
            conflux_http_server_config
            conflux_options
            Catch2::Catch2WithMain
    )

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_config_compile_fail_global_build_uring_flags
        SOURCE http_server_config_compile_fail_global_build_uring_flags.cxx
        TEST http-server-config/compile-fail-global-build-uring-flags
        LINK conflux_http_server_config conflux_options
        LABELS http compile-fail
        EXPECT "build_uring_flags")

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_config_compile_fail_global_flags_str
        SOURCE http_server_config_compile_fail_global_flags_str.cxx
        TEST http-server-config/compile-fail-global-flags-str
        LINK conflux_http_server_config conflux_options
        LABELS http compile-fail
        EXPECT "flags_str")

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_config_compile_fail_global_wq_fd_for_ring
        SOURCE http_server_config_compile_fail_global_wq_fd_for_ring.cxx
        TEST http-server-config/compile-fail-global-wq-fd-for-ring
        LINK conflux_http_server_config conflux_options
        LABELS http compile-fail
        EXPECT "wq_fd_for_ring")
endif()
