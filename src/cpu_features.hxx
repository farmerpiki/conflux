#pragma once

#if !defined(CONFLUX_ENABLE_CPU_DISPATCH)
	#define CONFLUX_ENABLE_CPU_DISPATCH 0
#endif
#if !defined(CONFLUX_CPU_FEATURE_PROBES_RUNTIME)
	#define CONFLUX_CPU_FEATURE_PROBES_RUNTIME CONFLUX_ENABLE_CPU_DISPATCH
#endif

extern "C" {

[[nodiscard]] bool conflux_cpu_supports_avx2() noexcept;
[[nodiscard]] bool conflux_cpu_supports_aesni_pclmul_sse41() noexcept;
}
