#include <cstddef>
#include <simd>

namespace cf::json_simd_std {
namespace dp = std::simd;

using vec_t = dp::vec<signed char>;
using mask_t = typename vec_t::mask_type;

inline constexpr std::size_t width = vec_t::size;

[[nodiscard]] inline vec_t load(
	char const *p) noexcept {
	auto const *u = reinterpret_cast<signed char const *>(p);
	return dp::unchecked_load<vec_t>(u, static_cast<std::ptrdiff_t>(width));
}

[[nodiscard]] inline std::size_t first_true(
	mask_t m) noexcept {
	return static_cast<std::size_t>(dp::reduce_min_index(m));
}

template<bool AsciiOnly>
std::size_t scan_dump_safe_run_impl(
	char const *p,
	std::size_t n) noexcept {
	std::size_t i = 0;
	vec_t const v_quote(static_cast<signed char>('"'));
	vec_t const v_back(static_cast<signed char>('\\'));
	vec_t const v_lim(static_cast<signed char>(0x20));
	vec_t const v_zero(static_cast<signed char>(0));
	while (i + width <= n) {
		auto v = load(p + i);
		auto mix = (v == v_quote) | (v == v_back);
		if constexpr (AsciiOnly) {
			mix = mix | (v < v_lim);
		} else {
			mix = mix | ((v >= v_zero) & (v < v_lim));
		}
		if (dp::any_of(mix)) {
			return i + first_true(mix);
		}
		i += width;
	}
	for (; i < n; ++i) {
		auto const c = static_cast<unsigned char>(p[i]);
		if (c == '"' || c == '\\' || c < 0x20U) {
			return i;
		}
		if (AsciiOnly && c >= 0x80U) {
			return i;
		}
	}
	return n;
}

} // namespace cf::json_simd_std

using vec_t = cf::json_simd_std::vec_t;
static constexpr std::size_t W = cf::json_simd_std::width;

extern "C" {
std::size_t conflux_json_scan_str_until_special_stdsimd(
	char const *p,
	std::size_t n) noexcept {
	std::size_t i = 0;
	vec_t const v_quote(static_cast<signed char>('"'));
	vec_t const v_back(static_cast<signed char>('\\'));
	vec_t const v_lim(static_cast<signed char>(0x20));
	while (i + W <= n) {
		auto v = cf::json_simd_std::load(p + i);
		auto mask = (v == v_quote) | (v == v_back) | (v < v_lim);
		if (std::simd::any_of(mask)) {
			return i + cf::json_simd_std::first_true(mask);
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
	return ascii_only != 0 ? cf::json_simd_std::scan_dump_safe_run_impl<true>(p, n) :
							 cf::json_simd_std::scan_dump_safe_run_impl<false>(p, n);
}
}
