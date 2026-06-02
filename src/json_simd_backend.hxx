#pragma once

#include <cstddef>

#include "cpu_features.hxx"

#if defined(CONFLUX_JSON_HAS_STDSIMD)
extern "C" {
std::size_t conflux_json_scan_str_until_special_stdsimd(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdsimd(char const *, std::size_t, int) noexcept;
}
#elif (defined(__x86_64__) || defined(_M_X64)) && !defined(__cpp_impl_reflection)
	#include <immintrin.h>
	#ifndef CONFLUX_JSON_DISABLE_SIMD
		#define CONFLUX_JSON_HAS_SSE2 1
		#if defined(__AVX2__)
			#define CONFLUX_JSON_HAS_AVX2 1
		#endif
	#endif
#endif
