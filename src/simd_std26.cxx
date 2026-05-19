#include <cstddef>
#include <simd>
#include <span>

using vec_t = std::simd::vec<unsigned char>;
static constexpr std::size_t W = vec_t::size;

extern "C" {
void conflux_ascii_lower_inplace_stdsimd(
	char *p,
	std::size_t n) noexcept {
	auto *u = reinterpret_cast<unsigned char *>(p);
	vec_t const va(static_cast<unsigned char>('A'));
	vec_t const vz(static_cast<unsigned char>('Z'));
	vec_t const bit(static_cast<unsigned char>(0x20));

	std::size_t i = 0;
	for (; i + W <= n; i += W) {
		auto v = std::simd::unchecked_load<vec_t>(std::span<unsigned char>(u + i, W));
		auto upper = (v >= va) & (v <= vz);
		v = std::simd::select(upper, v | bit, v);
		std::simd::unchecked_store(v, std::span<unsigned char>(u + i, W));
	}
	for (; i < n; ++i) {
		if (u[i] >= 'A' && u[i] <= 'Z') {
			u[i] |= 0x20U;
		}
	}
}
int conflux_constant_time_eq_stdsimd(
	unsigned char const *a,
	unsigned char const *b,
	std::size_t n) noexcept {
	vec_t acc(static_cast<unsigned char>(0));
	std::size_t i = 0;
	for (; i + W <= n; i += W) {
		auto va = std::simd::unchecked_load<vec_t>(std::span<unsigned char const>(a + i, W));
		auto vb = std::simd::unchecked_load<vec_t>(std::span<unsigned char const>(b + i, W));
		acc |= va ^ vb;
	}
	unsigned char tail = 0;
	for (; i < n; ++i) {
		tail = static_cast<unsigned char>(tail | (a[i] ^ b[i]));
	}
	return std::simd::all_of(acc == vec_t(static_cast<unsigned char>(0))) && tail == 0 ? 1 : 0;
}
std::size_t conflux_url_scan_plain_run_stdsimd(
	char const *p,
	std::size_t n,
	int plus_is_special) noexcept {
	auto const *u = reinterpret_cast<unsigned char const *>(p);
	vec_t const pct(static_cast<unsigned char>('%'));
	vec_t const plus(static_cast<unsigned char>('+'));

	std::size_t i = 0;
	for (; i + W <= n; i += W) {
		auto v = std::simd::unchecked_load<vec_t>(std::span<unsigned char const>(u + i, W));
		auto m = (v == pct);
		if (plus_is_special != 0) {
			m = m | (v == plus);
		}
		if (std::simd::any_of(m)) {
			return i + static_cast<std::size_t>(std::simd::reduce_min_index(m));
		}
	}
	for (; i < n; ++i) {
		if (p[i] == '%' || (plus_is_special != 0 && p[i] == '+')) {
			return i;
		}
	}
	return n;
}
void conflux_ws_unmask_stdsimd(
	unsigned char *data,
	std::size_t n,
	unsigned char const *mask4) noexcept {
	alignas(64) unsigned char repeated[W];
	for (std::size_t i = 0; i < W; ++i) {
		repeated[i] = mask4[i & 3];
	}
	auto const m = std::simd::unchecked_load<vec_t>(std::span<unsigned char const>(repeated, W));

	std::size_t i = 0;
	for (; i + W <= n; i += W) {
		auto v = std::simd::unchecked_load<vec_t>(std::span<unsigned char>(data + i, W));
		std::simd::unchecked_store(v ^ m, std::span<unsigned char>(data + i, W));
	}
	for (; i < n; ++i) {
		data[i] ^= mask4[i & 3];
	}
}
} // extern "C"
