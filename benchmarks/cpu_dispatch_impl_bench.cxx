#include "cpu_features.hxx"

import std;
import conflux.crypto;
import conflux.types;
import conflux.utils;
import bench_common;

#if defined(CONFLUX_BENCH_HAS_STDSIMD)
extern "C" {
void conflux_ascii_lower_inplace_stdsimd(char *, std::size_t) noexcept;
int conflux_constant_time_eq_stdsimd(unsigned char const *, unsigned char const *, std::size_t) noexcept;
std::size_t conflux_url_scan_plain_run_stdsimd(char const *, std::size_t, int) noexcept;
void conflux_ws_unmask_stdsimd(unsigned char *, std::size_t, unsigned char const *) noexcept;
}
#endif

#if defined(CONFLUX_BENCH_HAS_STDX_VARIANTS)
extern "C" {
void conflux_ascii_lower_inplace_stdx_sse2(char *, std::size_t) noexcept;
int conflux_constant_time_eq_stdx_sse2(unsigned char const *, unsigned char const *, std::size_t) noexcept;
std::size_t conflux_url_scan_plain_run_stdx_sse2(char const *, std::size_t, int) noexcept;
void conflux_ws_unmask_stdx_sse2(unsigned char *, std::size_t, unsigned char const *) noexcept;
void conflux_ascii_lower_inplace_stdx_avx2(char *, std::size_t) noexcept;
int conflux_constant_time_eq_stdx_avx2(unsigned char const *, unsigned char const *, std::size_t) noexcept;
std::size_t conflux_url_scan_plain_run_stdx_avx2(char const *, std::size_t, int) noexcept;
void conflux_ws_unmask_stdx_avx2(unsigned char *, std::size_t, unsigned char const *) noexcept;
}
#endif

#if defined(CONFLUX_BENCH_HAS_STD26_VARIANTS)
extern "C" {
void conflux_ascii_lower_inplace_std26_sse2(char *, std::size_t) noexcept;
int conflux_constant_time_eq_std26_sse2(unsigned char const *, unsigned char const *, std::size_t) noexcept;
std::size_t conflux_url_scan_plain_run_std26_sse2(char const *, std::size_t, int) noexcept;
void conflux_ws_unmask_std26_sse2(unsigned char *, std::size_t, unsigned char const *) noexcept;
void conflux_ascii_lower_inplace_std26_avx2(char *, std::size_t) noexcept;
int conflux_constant_time_eq_std26_avx2(unsigned char const *, unsigned char const *, std::size_t) noexcept;
std::size_t conflux_url_scan_plain_run_std26_avx2(char const *, std::size_t, int) noexcept;
void conflux_ws_unmask_std26_avx2(unsigned char *, std::size_t, unsigned char const *) noexcept;
}
#endif

#if defined(CONFLUX_BENCH_HAS_JSON_STDSIMD)
extern "C" {
std::size_t conflux_json_scan_str_until_special_stdsimd(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdsimd(char const *, std::size_t, int) noexcept;
}
#endif

#if defined(CONFLUX_BENCH_HAS_JSON_STDX_VARIANTS)
extern "C" {
std::size_t conflux_json_scan_str_until_special_stdx_sse2(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdx_sse2(char const *, std::size_t, int) noexcept;
std::size_t conflux_json_scan_str_until_special_stdx_avx2(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdx_avx2(char const *, std::size_t, int) noexcept;
}
#endif

#if defined(CONFLUX_BENCH_HAS_JSON_INTRIN_VARIANTS)
extern "C" {
std::size_t conflux_json_scan_str_until_special_intrin_sse2(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_intrin_sse2(char const *, std::size_t, int) noexcept;
std::size_t conflux_json_scan_str_until_special_intrin_avx2(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_intrin_avx2(char const *, std::size_t, int) noexcept;
}
#endif

#if defined(CONFLUX_BENCH_HAS_JSON_STD26_VARIANTS)
extern "C" {
std::size_t conflux_json_scan_str_until_special_std26_sse2(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_std26_sse2(char const *, std::size_t, int) noexcept;
std::size_t conflux_json_scan_str_until_special_std26_avx2(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_std26_avx2(char const *, std::size_t, int) noexcept;
}
#endif

#if defined(CONFLUX_BENCH_HAS_AESNI)
extern "C" {
int conflux_aes_gcm_encrypt_aesni(
	unsigned char const *,
	unsigned char const *,
	unsigned char const *,
	std::size_t,
	unsigned char const *,
	std::size_t,
	unsigned char *);
}
#endif

namespace {

bool g_json = false;
bool g_first = true;

#if defined(__GNUC__) || defined(__clang__)
template<typename T>
void bench_keep(
	T const &v) noexcept {
	asm volatile("" : : "g"(&v) : "memory");
}
inline void bench_keep_ptr(
	void const *p) noexcept {
	asm volatile("" : : "r"(p) : "memory");
}
#else
template<typename T>
void bench_keep(
	T const &v) noexcept {
	(void)v;
}
inline void bench_keep_ptr(
	void const *p) noexcept {
	(void)p;
}
#endif

template<typename F>
BenchStats measure(
	std::string_view variant,
	F &&fn,
	std::size_t warmup,
	std::size_t iters,
	std::size_t batch,
	std::size_t bytes = 0) {
	iters = std::max(iters, std::size_t{1});
	batch = std::max(batch, std::size_t{1});
	for (std::size_t i = 0; i < warmup * batch; ++i) {
		fn();
	}
	std::vector<std::uint64_t> samples;
	samples.reserve(iters);
	std::uint64_t total = 0;
	for (std::size_t i = 0; i < iters; ++i) {
		std::uint64_t const t0 = bench_now_ns();
		for (std::size_t j = 0; j < batch; ++j) {
			fn();
		}
		std::uint64_t const elapsed = bench_now_ns() - t0;
		total += elapsed;
		samples.push_back(elapsed);
	}
	std::ranges::sort(samples);
	double const med = static_cast<double>(samples[iters / 2]) / static_cast<double>(batch);
	double const mbs = (bytes > 0 && med > 0.0) ? static_cast<double>(bytes) / (med / 1e9) / (1024.0 * 1024.0) : 0.0;
	return {
		.config = {},
		.variant = variant,
		.iterations = iters * batch,
		.total_ns = total,
		.ns_per_iter = med,
		.throughput = mbs,
	};
}

void emit(
	BenchStats s) {
	if (g_json) {
		bench_print(s, true, g_first);
		g_first = false;
	} else if (s.throughput > 0.0) {
		std::println("[cpu-dispatch-impl] {:<42} {:>10.2f} ns  {:>9.1f} MB/s", s.variant, s.ns_per_iter, s.throughput);
	} else {
		std::println("[cpu-dispatch-impl] {:<42} {:>10.2f} ns", s.variant, s.ns_per_iter);
	}
}

[[nodiscard]] std::size_t scalar_json_scan_str_until_special(
	char const *p,
	std::size_t n) noexcept {
	for (std::size_t i = 0; i < n; ++i) {
		auto const c = static_cast<unsigned char>(p[i]);
		if (c == '"' || c == '\\' || c < 0x20U || c >= 0x80U) {
			return i;
		}
	}
	return n;
}

[[nodiscard]] std::size_t scalar_json_scan_dump_safe_run(
	char const *p,
	std::size_t n,
	bool ascii_only) noexcept {
	for (std::size_t i = 0; i < n; ++i) {
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

void scalar_ascii_lower_inplace(
	char *p,
	std::size_t n) noexcept {
	for (std::size_t i = 0; i < n; ++i) {
		auto const u = static_cast<unsigned char>(p[i]);
		p[i] = static_cast<char>(u >= 'A' && u <= 'Z' ? u | 0x20U : u);
	}
}

[[nodiscard]] int scalar_constant_time_eq(
	unsigned char const *a,
	unsigned char const *b,
	std::size_t n) noexcept {
	unsigned char acc = 0;
	for (std::size_t i = 0; i < n; ++i) {
		acc = static_cast<unsigned char>(acc | (a[i] ^ b[i]));
	}
	return acc == 0 ? 1 : 0;
}

[[nodiscard]] std::size_t scalar_url_scan_plain_run(
	char const *p,
	std::size_t n,
	bool plus_is_special) noexcept {
	for (std::size_t i = 0; i < n; ++i) {
		if (p[i] == '%' || (plus_is_special && p[i] == '+')) {
			return i;
		}
	}
	return n;
}

void scalar_ws_unmask(
	unsigned char *p,
	std::size_t n,
	unsigned char const key[4]) noexcept {
	for (std::size_t i = 0; i < n; ++i) {
		p[i] = static_cast<unsigned char>(p[i] ^ key[i & 3U]);
	}
}

void bench_json_scan(
	BenchArgs const &cfg) {
	[[maybe_unused]] bool const has_avx2 = conflux_cpu_supports_avx2();
	for (std::size_t sz: {64UZ, 256UZ, 4096UZ, 65536UZ}) {
		std::string s(sz, 'a');
		auto const batch = sz <= 256 ? 200UZ : 20UZ;
		auto const warmup = std::max(cfg.warmup / batch / 8UZ, 1UZ);
		auto const iters = std::max(cfg.iterations / batch / 8UZ, 1UZ);

		emit(measure(
			std::format("json_scan_str/scalar/{}", sz),
			[&] {
				auto r = scalar_json_scan_str_until_special(s.data(), s.size());
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
#if defined(CONFLUX_BENCH_HAS_JSON_STDSIMD)
		if (has_avx2) {
			emit(measure(
				std::format("json_scan_str/direct-simd/{}", sz),
				[&] {
					auto r = conflux_json_scan_str_until_special_stdsimd(s.data(), s.size());
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_JSON_STDX_VARIANTS)
		emit(measure(
			std::format("json_scan_str/stdx-sse2/{}", sz),
			[&] {
				auto r = conflux_json_scan_str_until_special_stdx_sse2(s.data(), s.size());
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("json_scan_str/stdx-avx2/{}", sz),
				[&] {
					auto r = conflux_json_scan_str_until_special_stdx_avx2(s.data(), s.size());
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_JSON_STD26_VARIANTS)
		emit(measure(
			std::format("json_scan_str/std26-sse2/{}", sz),
			[&] {
				auto r = conflux_json_scan_str_until_special_std26_sse2(s.data(), s.size());
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("json_scan_str/std26-avx2/{}", sz),
				[&] {
					auto r = conflux_json_scan_str_until_special_std26_avx2(s.data(), s.size());
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_JSON_INTRIN_VARIANTS)
		emit(measure(
			std::format("json_scan_str/intrin-sse2/{}", sz),
			[&] {
				auto r = conflux_json_scan_str_until_special_intrin_sse2(s.data(), s.size());
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("json_scan_str/intrin-avx2/{}", sz),
				[&] {
					auto r = conflux_json_scan_str_until_special_intrin_avx2(s.data(), s.size());
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
		emit(measure(
			std::format("json_dump_scan/scalar_ascii/{}", sz),
			[&] {
				auto r = scalar_json_scan_dump_safe_run(s.data(), s.size(), true);
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
#if defined(CONFLUX_BENCH_HAS_JSON_STDSIMD)
		if (has_avx2) {
			emit(measure(
				std::format("json_dump_scan/direct-simd_ascii/{}", sz),
				[&] {
					auto r = conflux_json_scan_dump_safe_run_stdsimd(s.data(), s.size(), 1);
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_JSON_STDX_VARIANTS)
		emit(measure(
			std::format("json_dump_scan/stdx-sse2_ascii/{}", sz),
			[&] {
				auto r = conflux_json_scan_dump_safe_run_stdx_sse2(s.data(), s.size(), 1);
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("json_dump_scan/stdx-avx2_ascii/{}", sz),
				[&] {
					auto r = conflux_json_scan_dump_safe_run_stdx_avx2(s.data(), s.size(), 1);
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_JSON_STD26_VARIANTS)
		emit(measure(
			std::format("json_dump_scan/std26-sse2_ascii/{}", sz),
			[&] {
				auto r = conflux_json_scan_dump_safe_run_std26_sse2(s.data(), s.size(), 1);
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("json_dump_scan/std26-avx2_ascii/{}", sz),
				[&] {
					auto r = conflux_json_scan_dump_safe_run_std26_avx2(s.data(), s.size(), 1);
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_JSON_INTRIN_VARIANTS)
		emit(measure(
			std::format("json_dump_scan/intrin-sse2_ascii/{}", sz),
			[&] {
				auto r = conflux_json_scan_dump_safe_run_intrin_sse2(s.data(), s.size(), 1);
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("json_dump_scan/intrin-avx2_ascii/{}", sz),
				[&] {
					auto r = conflux_json_scan_dump_safe_run_intrin_avx2(s.data(), s.size(), 1);
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
	}
}

void bench_utils(
	BenchArgs const &cfg) {
#if defined(CONFLUX_BENCH_HAS_STDSIMD)
	[[maybe_unused]] bool const has_avx2 = conflux_cpu_supports_avx2();
#else
	[[maybe_unused]] bool const has_avx2 = false;
#endif
	std::array<unsigned char, 4> const ws_key{0x12U, 0x34U, 0x56U, 0x78U};
	for (std::size_t sz: {64UZ, 256UZ, 4096UZ, 65536UZ}) {
		std::string lower_src(sz, 'X');
		std::string lower_buf(sz, 'X');
		std::string eq_a(sz, 'q');
		std::string eq_b(sz, 'q');
		std::string url(sz, 'u');
		std::vector<unsigned char> ws_src(sz, 0xA5U);
		std::vector<unsigned char> ws_buf(sz);
		auto const batch = sz <= 256 ? 200UZ : 20UZ;
		auto const warmup = std::max(cfg.warmup / batch / 8UZ, 1UZ);
		auto const iters = std::max(cfg.iterations / batch / 8UZ, 1UZ);

		emit(measure(
			std::format("ascii_lower/scalar/{}", sz),
			[&] {
				lower_buf = lower_src;
				scalar_ascii_lower_inplace(lower_buf.data(), lower_buf.size());
				bench_keep_ptr(lower_buf.data());
			},
			warmup,
			iters,
			batch,
			sz));
#if defined(CONFLUX_BENCH_HAS_STDSIMD)
		if (has_avx2) {
			emit(measure(
				std::format("ascii_lower/direct-simd/{}", sz),
				[&] {
					lower_buf = lower_src;
					conflux_ascii_lower_inplace_stdsimd(lower_buf.data(), lower_buf.size());
					bench_keep_ptr(lower_buf.data());
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_STDX_VARIANTS)
		emit(measure(
			std::format("ascii_lower/stdx-sse2/{}", sz),
			[&] {
				lower_buf = lower_src;
				conflux_ascii_lower_inplace_stdx_sse2(lower_buf.data(), lower_buf.size());
				bench_keep_ptr(lower_buf.data());
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("ascii_lower/stdx-avx2/{}", sz),
				[&] {
					lower_buf = lower_src;
					conflux_ascii_lower_inplace_stdx_avx2(lower_buf.data(), lower_buf.size());
					bench_keep_ptr(lower_buf.data());
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_STD26_VARIANTS)
		emit(measure(
			std::format("ascii_lower/std26-sse2/{}", sz),
			[&] {
				lower_buf = lower_src;
				conflux_ascii_lower_inplace_std26_sse2(lower_buf.data(), lower_buf.size());
				bench_keep_ptr(lower_buf.data());
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("ascii_lower/std26-avx2/{}", sz),
				[&] {
					lower_buf = lower_src;
					conflux_ascii_lower_inplace_std26_avx2(lower_buf.data(), lower_buf.size());
					bench_keep_ptr(lower_buf.data());
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
		emit(measure(
			std::format("ascii_lower/public-dispatch/{}", sz),
			[&] {
				lower_buf = lower_src;
				ascii_lower_inplace(std::span<char>{lower_buf.data(), lower_buf.size()});
				bench_keep_ptr(lower_buf.data());
			},
			warmup,
			iters,
			batch,
			sz));

		emit(measure(
			std::format("ct_eq/scalar/{}", sz),
			[&] {
				auto r = scalar_constant_time_eq(
					reinterpret_cast<unsigned char const *>(eq_a.data()),
					reinterpret_cast<unsigned char const *>(eq_b.data()),
					eq_a.size());
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
#if defined(CONFLUX_BENCH_HAS_STDSIMD)
		if (has_avx2) {
			emit(measure(
				std::format("ct_eq/direct-simd/{}", sz),
				[&] {
					auto r = conflux_constant_time_eq_stdsimd(
						reinterpret_cast<unsigned char const *>(eq_a.data()),
						reinterpret_cast<unsigned char const *>(eq_b.data()),
						eq_a.size());
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_STDX_VARIANTS)
		emit(measure(
			std::format("ct_eq/stdx-sse2/{}", sz),
			[&] {
				auto r = conflux_constant_time_eq_stdx_sse2(
					reinterpret_cast<unsigned char const *>(eq_a.data()),
					reinterpret_cast<unsigned char const *>(eq_b.data()),
					eq_a.size());
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("ct_eq/stdx-avx2/{}", sz),
				[&] {
					auto r = conflux_constant_time_eq_stdx_avx2(
						reinterpret_cast<unsigned char const *>(eq_a.data()),
						reinterpret_cast<unsigned char const *>(eq_b.data()),
						eq_a.size());
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_STD26_VARIANTS)
		emit(measure(
			std::format("ct_eq/std26-sse2/{}", sz),
			[&] {
				auto r = conflux_constant_time_eq_std26_sse2(
					reinterpret_cast<unsigned char const *>(eq_a.data()),
					reinterpret_cast<unsigned char const *>(eq_b.data()),
					eq_a.size());
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("ct_eq/std26-avx2/{}", sz),
				[&] {
					auto r = conflux_constant_time_eq_std26_avx2(
						reinterpret_cast<unsigned char const *>(eq_a.data()),
						reinterpret_cast<unsigned char const *>(eq_b.data()),
						eq_a.size());
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
		emit(measure(
			std::format("ct_eq/public-dispatch/{}", sz),
			[&] {
				auto r = constant_time_eq(eq_a, eq_b);
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));

		emit(measure(
			std::format("url_scan/scalar/{}", sz),
			[&] {
				auto r = scalar_url_scan_plain_run(url.data(), url.size(), true);
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
#if defined(CONFLUX_BENCH_HAS_STDSIMD)
		if (has_avx2) {
			emit(measure(
				std::format("url_scan/direct-simd/{}", sz),
				[&] {
					auto r = conflux_url_scan_plain_run_stdsimd(url.data(), url.size(), 1);
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_STDX_VARIANTS)
		emit(measure(
			std::format("url_scan/stdx-sse2/{}", sz),
			[&] {
				auto r = conflux_url_scan_plain_run_stdx_sse2(url.data(), url.size(), 1);
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("url_scan/stdx-avx2/{}", sz),
				[&] {
					auto r = conflux_url_scan_plain_run_stdx_avx2(url.data(), url.size(), 1);
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_STD26_VARIANTS)
		emit(measure(
			std::format("url_scan/std26-sse2/{}", sz),
			[&] {
				auto r = conflux_url_scan_plain_run_std26_sse2(url.data(), url.size(), 1);
				bench_keep(r);
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("url_scan/std26-avx2/{}", sz),
				[&] {
					auto r = conflux_url_scan_plain_run_std26_avx2(url.data(), url.size(), 1);
					bench_keep(r);
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif

		emit(measure(
			std::format("ws_unmask/scalar/{}", sz),
			[&] {
				std::ranges::copy(ws_src, ws_buf.begin());
				scalar_ws_unmask(ws_buf.data(), ws_buf.size(), ws_key.data());
				bench_keep_ptr(ws_buf.data());
			},
			warmup,
			iters,
			batch,
			sz));
#if defined(CONFLUX_BENCH_HAS_STDSIMD)
		if (has_avx2) {
			emit(measure(
				std::format("ws_unmask/direct-simd/{}", sz),
				[&] {
					std::ranges::copy(ws_src, ws_buf.begin());
					conflux_ws_unmask_stdsimd(ws_buf.data(), ws_buf.size(), ws_key.data());
					bench_keep_ptr(ws_buf.data());
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_STDX_VARIANTS)
		emit(measure(
			std::format("ws_unmask/stdx-sse2/{}", sz),
			[&] {
				std::ranges::copy(ws_src, ws_buf.begin());
				conflux_ws_unmask_stdx_sse2(ws_buf.data(), ws_buf.size(), ws_key.data());
				bench_keep_ptr(ws_buf.data());
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("ws_unmask/stdx-avx2/{}", sz),
				[&] {
					std::ranges::copy(ws_src, ws_buf.begin());
					conflux_ws_unmask_stdx_avx2(ws_buf.data(), ws_buf.size(), ws_key.data());
					bench_keep_ptr(ws_buf.data());
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
#if defined(CONFLUX_BENCH_HAS_STD26_VARIANTS)
		emit(measure(
			std::format("ws_unmask/std26-sse2/{}", sz),
			[&] {
				std::ranges::copy(ws_src, ws_buf.begin());
				conflux_ws_unmask_std26_sse2(ws_buf.data(), ws_buf.size(), ws_key.data());
				bench_keep_ptr(ws_buf.data());
			},
			warmup,
			iters,
			batch,
			sz));
		if (has_avx2) {
			emit(measure(
				std::format("ws_unmask/std26-avx2/{}", sz),
				[&] {
					std::ranges::copy(ws_src, ws_buf.begin());
					conflux_ws_unmask_std26_avx2(ws_buf.data(), ws_buf.size(), ws_key.data());
					bench_keep_ptr(ws_buf.data());
				},
				warmup,
				iters,
				batch,
				sz));
		}
#endif
	}
}

void bench_aes(
	BenchArgs const &cfg) {
#if defined(CONFLUX_BENCH_HAS_AESNI)
	if (!conflux_cpu_supports_aesni_pclmul_sse41()) {
		if (!g_json) {
			std::println("[cpu-dispatch-impl] AES-NI direct variants skipped: CPU lacks aes+pclmul+ssse3+sse4.1");
		}
		return;
	}

	std::array<unsigned char, 32> key{};
	std::array<unsigned char, 12> iv{};
	std::array<unsigned char, 16> aad{};
	crypto_random_bytes(key);
	crypto_random_bytes(iv);
	crypto_random_bytes(aad);

	for (std::size_t sz: {256UZ, 4096UZ, 65536UZ}) {
		std::vector<unsigned char> pt(sz);
		std::vector<unsigned char> out(sz + 16UZ);
		crypto_random_bytes(pt);
		auto const warmup = std::max(cfg.warmup / 20UZ, 1UZ);
		auto const iters = std::max(cfg.iterations / 20UZ, 1UZ);

		emit(measure(
			std::format("aes_gcm_encrypt/direct-aesni/{}", sz),
			[&] {
				auto const rc = conflux_aes_gcm_encrypt_aesni(
					key.data(),
					iv.data(),
					pt.data(),
					pt.size(),
					aad.data(),
					aad.size(),
					out.data());
				bench_keep(rc);
				bench_keep_ptr(out.data());
			},
			warmup,
			iters,
			1,
			sz));
		emit(measure(
			std::format("aes_gcm_encrypt/public-dispatch/{}", sz),
			[&] {
				auto ct = aes_gcm_encrypt(key, iv, pt, aad).value();
				bench_keep_ptr(ct.data());
			},
			warmup,
			iters,
			1,
			sz));
	}
#else
	(void)cfg;
#endif
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"cpu-dispatch-impl","parser":"standard","configs":[{"name":"default","extra":{"requires":"CONFLUX_ENABLE_CPU_DISPATCH"},"args":["--iterations","80000","--warmup","12000"]}]})");
	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	g_json = cfg.json_out;

	if (!g_json) {
		std::println(
			"[cpu-dispatch-impl] avx2={} aesni_pclmul_sse41={}",
			conflux_cpu_supports_avx2(),
			conflux_cpu_supports_aesni_pclmul_sse41());
	}

	bench_json_scan(cfg);
	bench_utils(cfg);
	bench_aes(cfg);
}
