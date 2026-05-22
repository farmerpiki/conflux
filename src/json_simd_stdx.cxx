#include <cstddef>
#include <experimental/simd>

namespace stdx = std::experimental::parallelism_v2;
using vec_t = stdx::native_simd<signed char>;
static constexpr std::size_t W = vec_t::size();

#ifndef CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD
	#define CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD conflux_json_scan_str_until_special_stdsimd
#endif
#ifndef CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD
	#define CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD conflux_json_scan_dump_safe_run_stdsimd
#endif

extern "C" {
std::size_t CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_STDSIMD(
	char const *p,
	std::size_t n) noexcept {
	std::size_t i = 0;
	vec_t const v_quote('"');
	vec_t const v_back('\\');
	vec_t const v_lim(0x20);
	while (i + W <= n) {
		vec_t v(reinterpret_cast<signed char const *>(p + i), stdx::element_aligned);
		auto mask = (v == v_quote) | (v == v_back) | (v < v_lim);
		if (stdx::any_of(mask)) {
			return i + static_cast<std::size_t>(stdx::find_first_set(mask));
		}
		i += W;
	}
	for (; i < n; ++i) {
		auto const c = static_cast<unsigned char>(p[i]);
		if (c == '"' || c == '\\' || c < 0x20U || c >= 0x80U) {
			return i;
		}
	}
	return n;
}
std::size_t CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_STDSIMD(
	char const *p,
	std::size_t n,
	int ascii_only) noexcept {
	std::size_t i = 0;
	vec_t const v_quote('"');
	vec_t const v_back('\\');
	vec_t const v_lim(0x20);
	vec_t const v_zero(0);
	while (i + W <= n) {
		vec_t v(reinterpret_cast<signed char const *>(p + i), stdx::element_aligned);
		auto eq_q = (v == v_quote);
		auto eq_b = (v == v_back);
		decltype(eq_q) mix;
		if (ascii_only) {
			mix = eq_q | eq_b | (v < v_lim);
		} else {
			mix = eq_q | eq_b | ((v >= v_zero) & (v < v_lim));
		}
		if (stdx::any_of(mix)) {
			return i + static_cast<std::size_t>(stdx::find_first_set(mix));
		}
		i += W;
	}
	for (; i < n; ++i) {
		auto const c = static_cast<unsigned char>(p[i]);
		if (c == '"' || c == '\\' || c < 0x20U) {
			return i;
		}
		if (ascii_only && c >= 0x80U) {
			return i;
		}
	}
	return n;
}
}
