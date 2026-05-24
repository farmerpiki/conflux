#include <cstddef>

extern "C" {
std::size_t conflux_json_scan_str_until_special_stdsimd_avx2(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdsimd_avx2(char const *, std::size_t, int) noexcept;
std::size_t conflux_json_scan_str_until_special_stdsimd_sse2(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdsimd_sse2(char const *, std::size_t, int) noexcept;
}

namespace {

using scan_str_fn = std::size_t (*)(char const *, std::size_t) noexcept;
using scan_dump_fn = std::size_t (*)(char const *, std::size_t, int) noexcept;

[[maybe_unused]] std::size_t conflux_json_scan_str_until_special_scalar(
	char const *p,
	std::size_t n) noexcept {
	std::size_t i = 0;
	for (; i < n; ++i) {
		auto const c = static_cast<unsigned char>(p[i]);
		if (c == '"' || c == '\\' || c < 0x20U || c >= 0x80U) {
			return i;
		}
	}
	return n;
}

[[maybe_unused]] std::size_t conflux_json_scan_dump_safe_run_scalar(
	char const *p,
	std::size_t n,
	int ascii_only) noexcept {
	std::size_t i = 0;
	for (; i < n; ++i) {
		auto const c = static_cast<unsigned char>(p[i]);
		if (c == '"' || c == '\\' || c < 0x20U) {
			return i;
		}
		if (ascii_only != 0 && c >= 0x80U) {
			return i;
		}
	}
	return n;
}

} // namespace

extern "C" {

scan_str_fn resolve_conflux_json_scan_str_until_special() noexcept {
	__builtin_cpu_init();
	if (__builtin_cpu_supports("avx2") != 0) {
		return conflux_json_scan_str_until_special_stdsimd_avx2;
	}
#if defined(__x86_64__) || defined(_M_X64)
	return conflux_json_scan_str_until_special_stdsimd_sse2;
#else
	if (__builtin_cpu_supports("sse2") != 0) {
		return conflux_json_scan_str_until_special_stdsimd_sse2;
	}
	return conflux_json_scan_str_until_special_scalar;
#endif
}

scan_dump_fn resolve_conflux_json_scan_dump_safe_run() noexcept {
	__builtin_cpu_init();
	if (__builtin_cpu_supports("avx2") != 0) {
		return conflux_json_scan_dump_safe_run_stdsimd_avx2;
	}
#if defined(__x86_64__) || defined(_M_X64)
	return conflux_json_scan_dump_safe_run_stdsimd_sse2;
#else
	if (__builtin_cpu_supports("sse2") != 0) {
		return conflux_json_scan_dump_safe_run_stdsimd_sse2;
	}
	return conflux_json_scan_dump_safe_run_scalar;
#endif
}

std::size_t conflux_json_scan_str_until_special_stdsimd(char const *, std::size_t) noexcept
	__attribute__((ifunc("resolve_conflux_json_scan_str_until_special")));

std::size_t conflux_json_scan_dump_safe_run_stdsimd(char const *, std::size_t, int) noexcept
	__attribute__((ifunc("resolve_conflux_json_scan_dump_safe_run")));
}
