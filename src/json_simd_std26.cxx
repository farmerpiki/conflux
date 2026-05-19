#include <cstddef>
#include <simd>
#include <span>

using vec_t = std::simd::vec<signed char>;
static constexpr std::size_t W = vec_t::size;

extern "C" {
std::size_t conflux_json_scan_str_until_special_stdsimd(
	char const *p,
	std::size_t n) noexcept {
	std::size_t i = 0;
	vec_t const v_quote(static_cast<signed char>('"'));
	vec_t const v_back(static_cast<signed char>('\\'));
	vec_t const v_lim(static_cast<signed char>(0x20));
	while (i + W <= n) {
		auto const *u = reinterpret_cast<signed char const *>(p + i);
		auto v = std::simd::unchecked_load<vec_t>(std::span<signed char const>(u, W));
		auto mask = (v == v_quote) | (v == v_back) | (v < v_lim);
		if (std::simd::any_of(mask)) {
			return i + static_cast<std::size_t>(std::simd::reduce_min_index(mask));
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
	vec_t const v_quote(static_cast<signed char>('"'));
	vec_t const v_back(static_cast<signed char>('\\'));
	vec_t const v_lim(static_cast<signed char>(0x20));
	vec_t const v_zero(static_cast<signed char>(0));
	while (i + W <= n) {
		auto const *u = reinterpret_cast<signed char const *>(p + i);
		auto v = std::simd::unchecked_load<vec_t>(std::span<signed char const>(u, W));
		auto eq_q = (v == v_quote);
		auto eq_b = (v == v_back);
		decltype(eq_q) mix;
		if (ascii_only) {
			mix = eq_q | eq_b | (v < v_lim);
		} else {
			mix = eq_q | eq_b | ((v >= v_zero) & (v < v_lim));
		}
		if (std::simd::any_of(mix)) {
			return i + static_cast<std::size_t>(std::simd::reduce_min_index(mix));
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
