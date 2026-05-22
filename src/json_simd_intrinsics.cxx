#include <cstddef>
#include <immintrin.h>

#ifndef CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_INTRIN
	#define CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_INTRIN conflux_json_scan_str_until_special_intrin
#endif
#ifndef CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_INTRIN
	#define CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_INTRIN conflux_json_scan_dump_safe_run_intrin
#endif

extern "C" {

std::size_t CONFLUX_JSON_SCAN_STR_UNTIL_SPECIAL_INTRIN(
	char const *p,
	std::size_t n) noexcept {
	std::size_t i = 0;
#if CONFLUX_JSON_INTRIN_AVX2
	__m256i const v_quote = _mm256_set1_epi8('"');
	__m256i const v_back = _mm256_set1_epi8('\\');
	__m256i const v_lim = _mm256_set1_epi8(0x20);
	while (i + 32 <= n) {
		__m256i const v = _mm256_loadu_si256(reinterpret_cast<__m256i const *>(p + i));
		__m256i const eq_q = _mm256_cmpeq_epi8(v, v_quote);
		__m256i const eq_b = _mm256_cmpeq_epi8(v, v_back);
		__m256i const lt_l = _mm256_cmpgt_epi8(v_lim, v);
		__m256i const mix = _mm256_or_si256(_mm256_or_si256(eq_q, eq_b), lt_l);
		auto const mask = static_cast<unsigned>(_mm256_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<std::size_t>(__builtin_ctz(mask));
		}
		i += 32;
	}
#endif
	__m128i const v_quote128 = _mm_set1_epi8('"');
	__m128i const v_back128 = _mm_set1_epi8('\\');
	__m128i const v_lim128 = _mm_set1_epi8(0x20);
	while (i + 16 <= n) {
		__m128i const v = _mm_loadu_si128(reinterpret_cast<__m128i const *>(p + i));
		__m128i const eq_q = _mm_cmpeq_epi8(v, v_quote128);
		__m128i const eq_b = _mm_cmpeq_epi8(v, v_back128);
		__m128i const lt_lim = _mm_cmplt_epi8(v, v_lim128);
		__m128i const mix = _mm_or_si128(_mm_or_si128(eq_q, eq_b), lt_lim);
		auto const mask = static_cast<unsigned>(_mm_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<std::size_t>(__builtin_ctz(mask));
		}
		i += 16;
	}
	for (; i < n; ++i) {
		auto const c = static_cast<unsigned char>(p[i]);
		if (c == '"' || c == '\\' || c < 0x20U || c >= 0x80U) {
			return i;
		}
	}
	return n;
}

std::size_t CONFLUX_JSON_SCAN_DUMP_SAFE_RUN_INTRIN(
	char const *p,
	std::size_t n,
	int ascii_only) noexcept {
	std::size_t i = 0;
#if CONFLUX_JSON_INTRIN_AVX2
	__m256i const v_quote = _mm256_set1_epi8('"');
	__m256i const v_back = _mm256_set1_epi8('\\');
	__m256i const v_lim = _mm256_set1_epi8(0x20);
	__m256i const v_1f = _mm256_set1_epi8(0x1F);
	while (i + 32 <= n) {
		__m256i const v = _mm256_loadu_si256(reinterpret_cast<__m256i const *>(p + i));
		__m256i const eq_q = _mm256_cmpeq_epi8(v, v_quote);
		__m256i const eq_b = _mm256_cmpeq_epi8(v, v_back);
		__m256i mix = _mm256_or_si256(eq_q, eq_b);
		if (ascii_only != 0) {
			mix = _mm256_or_si256(mix, _mm256_cmpgt_epi8(v_lim, v));
		} else {
			mix = _mm256_or_si256(mix, _mm256_cmpeq_epi8(_mm256_min_epu8(v, v_1f), v));
		}
		auto const mask = static_cast<unsigned>(_mm256_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<std::size_t>(__builtin_ctz(mask));
		}
		i += 32;
	}
#endif
	__m128i const v_quote128 = _mm_set1_epi8('"');
	__m128i const v_back128 = _mm_set1_epi8('\\');
	__m128i const v_lim128 = _mm_set1_epi8(0x20);
	__m128i const v_1f128 = _mm_set1_epi8(0x1F);
	while (i + 16 <= n) {
		__m128i const v = _mm_loadu_si128(reinterpret_cast<__m128i const *>(p + i));
		__m128i const eq_q = _mm_cmpeq_epi8(v, v_quote128);
		__m128i const eq_b = _mm_cmpeq_epi8(v, v_back128);
		__m128i mix = _mm_or_si128(eq_q, eq_b);
		if (ascii_only != 0) {
			mix = _mm_or_si128(mix, _mm_cmplt_epi8(v, v_lim128));
		} else {
			mix = _mm_or_si128(mix, _mm_cmpeq_epi8(_mm_min_epu8(v, v_1f128), v));
		}
		auto const mask = static_cast<unsigned>(_mm_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<std::size_t>(__builtin_ctz(mask));
		}
		i += 16;
	}
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

} // extern "C"
