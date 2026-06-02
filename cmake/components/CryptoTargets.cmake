if(CONFLUX_WANT_CRYPTO)
conflux_add_module_library(conflux_crypto
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/crypto.cxx
)
target_link_libraries(conflux_crypto
    PRIVATE conflux_options
    PRIVATE conflux_cpu_features
    PUBLIC  conflux_types
)
if(CONFLUX_HAS_AESNI)
    message(STATUS "conflux: AES-NI + PCLMUL + SSSE3 path enabled")
    target_compile_definitions(conflux_crypto PRIVATE CONFLUX_CRYPTO_USE_AESNI=1)
    target_sources(conflux_crypto PRIVATE ${CONFLUX_SRC_ROOT}/crypto_aesni.cxx)
    conflux_apply_aesni_source_options("${CONFLUX_SRC_ROOT}/crypto_aesni.cxx")
endif()
endif()
