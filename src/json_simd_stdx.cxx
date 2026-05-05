#include <experimental/simd>
#include <cstddef>

namespace stdx = std::experimental::parallelism_v2;
using vec_t = stdx::native_simd<signed char>;
static constexpr std::size_t W = vec_t::size();

extern "C" {

std::size_t conflux_json_scan_str_until_special_stdsimd(
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

std::size_t conflux_json_scan_dump_safe_run_stdsimd(
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
		auto lt_l = (v < v_lim);
		decltype(eq_q) mix;
		if (ascii_only) {
			mix = eq_q | eq_b | lt_l;
		} else {
			auto ctrl_only = lt_l & !(v < v_zero);
			mix = eq_q | eq_b | ctrl_only;
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
