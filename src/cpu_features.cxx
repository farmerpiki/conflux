#include "cpu_features.hxx"

#if CONFLUX_CPU_FEATURE_PROBES_RUNTIME                                                   \
	&& (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) \
	&& (defined(__GNUC__) || defined(__clang__))
	#define CONFLUX_CPU_HAS_X86_BUILTINS 1
#endif

#if CONFLUX_CPU_FEATURE_PROBES_RUNTIME
namespace {

[[nodiscard]] bool detect_avx2_once() noexcept {
	#if defined(CONFLUX_CPU_HAS_X86_BUILTINS)
	__builtin_cpu_init();
	return __builtin_cpu_supports("avx2") != 0;
	#else
	return false;
	#endif
}

[[nodiscard]] bool detect_aesni_pclmul_sse41_once() noexcept {
	#if defined(CONFLUX_CPU_HAS_X86_BUILTINS)
	__builtin_cpu_init();
	return __builtin_cpu_supports("aes") != 0
		&& __builtin_cpu_supports("pclmul") != 0
		&& __builtin_cpu_supports("ssse3") != 0
		&& __builtin_cpu_supports("sse4.1") != 0;
	#else
	return false;
	#endif
}

} // namespace
#endif

extern "C" bool conflux_cpu_supports_avx2() noexcept {
#if !CONFLUX_CPU_FEATURE_PROBES_RUNTIME
	return true;
#else
	static bool const supported = detect_avx2_once();
	return supported;
#endif
}

extern "C" bool conflux_cpu_supports_aesni_pclmul_sse41() noexcept {
#if !CONFLUX_CPU_FEATURE_PROBES_RUNTIME
	return true;
#else
	static bool const supported = detect_aesni_pclmul_sse41_once();
	return supported;
#endif
}
