#include <cstddef>
#include <functional>
#include <simd>

namespace cf::simd_std {
namespace dp = std::simd;

template<class T>
using native = dp::vec<T>;

using byte_vec = native<unsigned char>;
using byte_mask = typename byte_vec::mask_type;

inline constexpr std::size_t byte_width = byte_vec::size;

[[nodiscard]] inline byte_vec load(
	unsigned char const *p) noexcept {
	return dp::unchecked_load<byte_vec>(p, static_cast<std::ptrdiff_t>(byte_width));
}

inline void store(
	unsigned char *p,
	byte_vec v) noexcept {
	dp::unchecked_store(v, p, static_cast<std::ptrdiff_t>(byte_width));
}

[[nodiscard]] inline std::size_t first_true(
	byte_mask m) noexcept {
	return static_cast<std::size_t>(dp::reduce_min_index(m));
}

[[nodiscard]] inline unsigned char reduce_or(
	byte_vec v) noexcept {
	return dp::reduce(v, std::bit_or<>{});
}

} // namespace cf::simd_std

using vec_t = cf::simd_std::byte_vec;
static constexpr std::size_t W = cf::simd_std::byte_width;

#ifndef CONFLUX_ASCII_LOWER_INPLACE_STDSIMD
	#define CONFLUX_ASCII_LOWER_INPLACE_STDSIMD conflux_ascii_lower_inplace_stdsimd
#endif
#ifndef CONFLUX_CONSTANT_TIME_EQ_STDSIMD
	#define CONFLUX_CONSTANT_TIME_EQ_STDSIMD conflux_constant_time_eq_stdsimd
#endif
#ifndef CONFLUX_URL_SCAN_PLAIN_RUN_STDSIMD
	#define CONFLUX_URL_SCAN_PLAIN_RUN_STDSIMD conflux_url_scan_plain_run_stdsimd
#endif
#ifndef CONFLUX_WS_UNMASK_STDSIMD
	#define CONFLUX_WS_UNMASK_STDSIMD conflux_ws_unmask_stdsimd
#endif

namespace {

template<bool PlusIsSpecial>
std::size_t url_scan_plain_run_impl(
	char const *p,
	std::size_t n) noexcept {
	auto const *u = reinterpret_cast<unsigned char const *>(p);
	vec_t const pct(static_cast<unsigned char>('%'));
	vec_t const plus(static_cast<unsigned char>('+'));

	std::size_t i = 0;
	for (; i + W <= n; i += W) {
		auto v = cf::simd_std::load(u + i);
		auto m = (v == pct);
		if constexpr (PlusIsSpecial) {
			m = m | (v == plus);
		}
		if (std::simd::any_of(m)) {
			return i + cf::simd_std::first_true(m);
		}
	}
	for (; i < n; ++i) {
		if (p[i] == '%' || (PlusIsSpecial && p[i] == '+')) {
			return i;
		}
	}
	return n;
}

} // namespace

extern "C" {
void CONFLUX_ASCII_LOWER_INPLACE_STDSIMD(
	char *p,
	std::size_t n) noexcept {
	auto *u = reinterpret_cast<unsigned char *>(p);
	vec_t const va(static_cast<unsigned char>('A'));
	vec_t const vz(static_cast<unsigned char>('Z'));
	vec_t const bit(static_cast<unsigned char>(0x20));

	std::size_t i = 0;
	for (; i + W <= n; i += W) {
		auto v = cf::simd_std::load(u + i);
		auto upper = (v >= va) & (v <= vz);
		v = std::simd::select(upper, v | bit, v);
		cf::simd_std::store(u + i, v);
	}
	for (; i < n; ++i) {
		if (u[i] >= 'A' && u[i] <= 'Z') {
			u[i] |= 0x20U;
		}
	}
}
int CONFLUX_CONSTANT_TIME_EQ_STDSIMD(
	unsigned char const *a,
	unsigned char const *b,
	std::size_t n) noexcept {
	vec_t acc(static_cast<unsigned char>(0));
	std::size_t i = 0;
	for (; i + W <= n; i += W) {
		auto va = cf::simd_std::load(a + i);
		auto vb = cf::simd_std::load(b + i);
		acc |= va ^ vb;
	}
	unsigned char tail = 0;
	for (; i < n; ++i) {
		tail = static_cast<unsigned char>(tail | (a[i] ^ b[i]));
	}
	return (cf::simd_std::reduce_or(acc) | tail) == 0 ? 1 : 0;
}
std::size_t CONFLUX_URL_SCAN_PLAIN_RUN_STDSIMD(
	char const *p,
	std::size_t n,
	int plus_is_special) noexcept {
	return plus_is_special != 0 ? url_scan_plain_run_impl<true>(p, n) : url_scan_plain_run_impl<false>(p, n);
}
void CONFLUX_WS_UNMASK_STDSIMD(
	unsigned char *data,
	std::size_t n,
	unsigned char const *mask4) noexcept {
	alignas(vec_t) unsigned char repeated[W];
	for (std::size_t i = 0; i < W; ++i) {
		repeated[i] = mask4[i & 3];
	}
	auto const m = cf::simd_std::load(repeated);

	std::size_t i = 0;
	for (; i + W <= n; i += W) {
		auto v = cf::simd_std::load(data + i);
		cf::simd_std::store(data + i, v ^ m);
	}
	for (; i < n; ++i) {
		data[i] ^= mask4[i & 3];
	}
}
} // extern "C"
