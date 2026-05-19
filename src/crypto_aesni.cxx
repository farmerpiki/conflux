#include <cstddef>
#include <cstdint>
#include <cstring>
#include <smmintrin.h>
#include <tmmintrin.h>
#include <wmmintrin.h>
inline static __m128i aes256_key_expand_assist_1(
	__m128i key,
	__m128i keygen) {
	keygen = _mm_shuffle_epi32(keygen, 0xFF);
	key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
	key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
	key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
	return _mm_xor_si128(key, keygen);
}
inline static __m128i aes256_key_expand_assist_2(
	__m128i key1,
	__m128i key2) {
	__m128i t = _mm_aeskeygenassist_si128(key1, 0);
	t = _mm_shuffle_epi32(t, 0xAA);
	key2 = _mm_xor_si128(key2, _mm_slli_si128(key2, 4));
	key2 = _mm_xor_si128(key2, _mm_slli_si128(key2, 4));
	key2 = _mm_xor_si128(key2, _mm_slli_si128(key2, 4));
	return _mm_xor_si128(key2, t);
}
struct AesniKey256 {
	__m128i rk[15];
};
static AesniKey256 aesni_expand_key(
	unsigned char const *key) {
	AesniKey256 ek{};
	__m128i k0 = _mm_loadu_si128(reinterpret_cast<__m128i const *>(key));
	__m128i k1 = _mm_loadu_si128(reinterpret_cast<__m128i const *>(key + 16));
	ek.rk[0] = k0;
	ek.rk[1] = k1;

	k0 = aes256_key_expand_assist_1(k0, _mm_aeskeygenassist_si128(k1, 0x01));
	ek.rk[2] = k0;
	k1 = aes256_key_expand_assist_2(k0, k1);
	ek.rk[3] = k1;

	k0 = aes256_key_expand_assist_1(k0, _mm_aeskeygenassist_si128(k1, 0x02));
	ek.rk[4] = k0;
	k1 = aes256_key_expand_assist_2(k0, k1);
	ek.rk[5] = k1;

	k0 = aes256_key_expand_assist_1(k0, _mm_aeskeygenassist_si128(k1, 0x04));
	ek.rk[6] = k0;
	k1 = aes256_key_expand_assist_2(k0, k1);
	ek.rk[7] = k1;

	k0 = aes256_key_expand_assist_1(k0, _mm_aeskeygenassist_si128(k1, 0x08));
	ek.rk[8] = k0;
	k1 = aes256_key_expand_assist_2(k0, k1);
	ek.rk[9] = k1;

	k0 = aes256_key_expand_assist_1(k0, _mm_aeskeygenassist_si128(k1, 0x10));
	ek.rk[10] = k0;
	k1 = aes256_key_expand_assist_2(k0, k1);
	ek.rk[11] = k1;

	k0 = aes256_key_expand_assist_1(k0, _mm_aeskeygenassist_si128(k1, 0x20));
	ek.rk[12] = k0;
	k1 = aes256_key_expand_assist_2(k0, k1);
	ek.rk[13] = k1;

	k0 = aes256_key_expand_assist_1(k0, _mm_aeskeygenassist_si128(k1, 0x40));
	ek.rk[14] = k0;

	return ek;
}
inline static __m128i aesni_encrypt_block(
	AesniKey256 const &ek,
	__m128i block) {
	block = _mm_xor_si128(block, ek.rk[0]);
	block = _mm_aesenc_si128(block, ek.rk[1]);
	block = _mm_aesenc_si128(block, ek.rk[2]);
	block = _mm_aesenc_si128(block, ek.rk[3]);
	block = _mm_aesenc_si128(block, ek.rk[4]);
	block = _mm_aesenc_si128(block, ek.rk[5]);
	block = _mm_aesenc_si128(block, ek.rk[6]);
	block = _mm_aesenc_si128(block, ek.rk[7]);
	block = _mm_aesenc_si128(block, ek.rk[8]);
	block = _mm_aesenc_si128(block, ek.rk[9]);
	block = _mm_aesenc_si128(block, ek.rk[10]);
	block = _mm_aesenc_si128(block, ek.rk[11]);
	block = _mm_aesenc_si128(block, ek.rk[12]);
	block = _mm_aesenc_si128(block, ek.rk[13]);
	return _mm_aesenclast_si128(block, ek.rk[14]);
}
// GCM ↔ PCLMULQDQ domain: reverse bits within each std::byte (no std::byte-swap)
inline static __m128i byte_bitrev(
	__m128i v) {
	__m128i const mask_lo = _mm_set1_epi8(0x0F);
	__m128i const lut = _mm_setr_epi8(0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE, 0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF);
	__m128i lo = _mm_and_si128(v, mask_lo);
	__m128i hi = _mm_and_si128(_mm_srli_epi16(v, 4), mask_lo);
	lo = _mm_shuffle_epi8(lut, lo);
	hi = _mm_shuffle_epi8(lut, hi);
	return _mm_or_si128(_mm_slli_epi16(lo, 4), hi);
}
// Reduce 256-bit GF(2^128) product [t1:t0] mod P(x)=x^128+x^7+x^2+x+1.
// t0 = low 128 bits, t1 = high 128 bits (std::byte-bitrev domain).
inline static __m128i reduce256(
	__m128i t0,
	__m128i t1) noexcept {
	__m128i const hi1 = _mm_or_si128(_mm_slli_epi64(t1, 1), _mm_slli_si128(_mm_srli_epi64(t1, 63), 8));
	__m128i const hi2 = _mm_or_si128(_mm_slli_epi64(t1, 2), _mm_slli_si128(_mm_srli_epi64(t1, 62), 8));
	__m128i const hi7 = _mm_or_si128(_mm_slli_epi64(t1, 7), _mm_slli_si128(_mm_srli_epi64(t1, 57), 8));
	__m128i r = _mm_xor_si128(_mm_xor_si128(t0, t1), _mm_xor_si128(hi1, _mm_xor_si128(hi2, hi7)));
	__m128i const hi_hi = _mm_srli_si128(t1, 8);
	__m128i const ov =
		_mm_xor_si128(_mm_srli_epi64(hi_hi, 63), _mm_xor_si128(_mm_srli_epi64(hi_hi, 62), _mm_srli_epi64(hi_hi, 57)));
	r = _mm_xor_si128(r, ov);
	return _mm_xor_si128(
		r,
		_mm_xor_si128(_mm_slli_epi64(ov, 1), _mm_xor_si128(_mm_slli_epi64(ov, 2), _mm_slli_epi64(ov, 7))));
}
// GF(2^128) multiply via Karatsuba: 3 PCLMULQDQ instead of 4.
// a_m = [*:a_lo XOR a_hi] (low 64 bits are the Karatsuba mixed factor; high 64 bits ignored).
// Pass precomputed a_m when available to avoid recomputing.
inline static __m128i gf128_mul_m(
	__m128i a,
	__m128i b,
	__m128i a_m,
	__m128i b_m) noexcept {
	__m128i t0 = _mm_clmulepi64_si128(a, b, 0x00);
	__m128i t1 = _mm_clmulepi64_si128(a, b, 0x11);
	__m128i tm = _mm_clmulepi64_si128(a_m, b_m, 0x00);
	tm = _mm_xor_si128(tm, _mm_xor_si128(t0, t1));
	t0 = _mm_xor_si128(t0, _mm_slli_si128(tm, 8));
	t1 = _mm_xor_si128(t1, _mm_srli_si128(tm, 8));
	return reduce256(t0, t1);
}
// Convenience wrapper — computes mixed factors inline (used for H-powers setup and tail).
inline static __m128i gf128_mul(
	__m128i a,
	__m128i b) noexcept {
	__m128i const a_m = _mm_xor_si128(a, _mm_srli_si128(a, 8));
	__m128i const b_m = _mm_xor_si128(b, _mm_srli_si128(b, 8));
	return gf128_mul_m(a, b, a_m, b_m);
}
// Per-std::thread cache of the last-used key schedule and H powers.
// Covers the common pattern: one key per std::thread, many operations.
struct KeyCtx {
	AesniKey256 ek;
	__m128i h_br, h2_br, h3_br, h4_br;
	__m128i h_m, h2_m, h3_m, h4_m; // Karatsuba mixed factors: low64 = H[63:0] XOR H[127:64]
	unsigned char key[32];
	bool valid{false};
};
static KeyCtx const &get_key_ctx(
	unsigned char const *key) {
	thread_local KeyCtx tl_key_ctx;
	if (tl_key_ctx.valid && std::memcmp(tl_key_ctx.key, key, 32) == 0) {
		return tl_key_ctx;
	}
	tl_key_ctx.ek = aesni_expand_key(key);
	__m128i const h = aesni_encrypt_block(tl_key_ctx.ek, _mm_setzero_si128());
	tl_key_ctx.h_br = byte_bitrev(h);
	tl_key_ctx.h2_br = gf128_mul(tl_key_ctx.h_br, tl_key_ctx.h_br);
	tl_key_ctx.h3_br = gf128_mul(tl_key_ctx.h2_br, tl_key_ctx.h_br);
	tl_key_ctx.h4_br = gf128_mul(tl_key_ctx.h2_br, tl_key_ctx.h2_br);
	auto const km = [](__m128i v) noexcept { return _mm_xor_si128(v, _mm_srli_si128(v, 8)); };
	tl_key_ctx.h_m = km(tl_key_ctx.h_br);
	tl_key_ctx.h2_m = km(tl_key_ctx.h2_br);
	tl_key_ctx.h3_m = km(tl_key_ctx.h3_br);
	tl_key_ctx.h4_m = km(tl_key_ctx.h4_br);
	std::memcpy(tl_key_ctx.key, key, 32);
	tl_key_ctx.valid = true;
	return tl_key_ctx;
}
// Register-only GCM counter increment (no memory round-trip).
// Bytes [12..15] hold a big-endian 32-bit counter; byteswap to std::logic_error, add 1, byteswap back.
inline static __m128i gcm_inc32_fast(
	__m128i ctr) noexcept {
	alignas(16) static constexpr uint8_t kBswap[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 15, 14, 13, 12};
	__m128i const shuf = _mm_load_si128(reinterpret_cast<__m128i const *>(kBswap));
	ctr = _mm_shuffle_epi8(ctr, shuf);
	ctr = _mm_add_epi32(ctr, _mm_setr_epi32(0, 0, 0, 1));
	return _mm_shuffle_epi8(ctr, shuf);
}
// Encrypt 4 independent CTR blocks simultaneously, hiding AES-NI pipeline latency.
// b0..b3 in: counter blocks. b0..b3 out: keystream blocks.
inline static void aesni_encrypt_4x(
	AesniKey256 const &ek,
	__m128i &b0,
	__m128i &b1,
	__m128i &b2,
	__m128i &b3) noexcept {
	b0 = _mm_xor_si128(b0, ek.rk[0]);
	b1 = _mm_xor_si128(b1, ek.rk[0]);
	b2 = _mm_xor_si128(b2, ek.rk[0]);
	b3 = _mm_xor_si128(b3, ek.rk[0]);
	b0 = _mm_aesenc_si128(b0, ek.rk[1]);
	b1 = _mm_aesenc_si128(b1, ek.rk[1]);
	b2 = _mm_aesenc_si128(b2, ek.rk[1]);
	b3 = _mm_aesenc_si128(b3, ek.rk[1]);
	b0 = _mm_aesenc_si128(b0, ek.rk[2]);
	b1 = _mm_aesenc_si128(b1, ek.rk[2]);
	b2 = _mm_aesenc_si128(b2, ek.rk[2]);
	b3 = _mm_aesenc_si128(b3, ek.rk[2]);
	b0 = _mm_aesenc_si128(b0, ek.rk[3]);
	b1 = _mm_aesenc_si128(b1, ek.rk[3]);
	b2 = _mm_aesenc_si128(b2, ek.rk[3]);
	b3 = _mm_aesenc_si128(b3, ek.rk[3]);
	b0 = _mm_aesenc_si128(b0, ek.rk[4]);
	b1 = _mm_aesenc_si128(b1, ek.rk[4]);
	b2 = _mm_aesenc_si128(b2, ek.rk[4]);
	b3 = _mm_aesenc_si128(b3, ek.rk[4]);
	b0 = _mm_aesenc_si128(b0, ek.rk[5]);
	b1 = _mm_aesenc_si128(b1, ek.rk[5]);
	b2 = _mm_aesenc_si128(b2, ek.rk[5]);
	b3 = _mm_aesenc_si128(b3, ek.rk[5]);
	b0 = _mm_aesenc_si128(b0, ek.rk[6]);
	b1 = _mm_aesenc_si128(b1, ek.rk[6]);
	b2 = _mm_aesenc_si128(b2, ek.rk[6]);
	b3 = _mm_aesenc_si128(b3, ek.rk[6]);
	b0 = _mm_aesenc_si128(b0, ek.rk[7]);
	b1 = _mm_aesenc_si128(b1, ek.rk[7]);
	b2 = _mm_aesenc_si128(b2, ek.rk[7]);
	b3 = _mm_aesenc_si128(b3, ek.rk[7]);
	b0 = _mm_aesenc_si128(b0, ek.rk[8]);
	b1 = _mm_aesenc_si128(b1, ek.rk[8]);
	b2 = _mm_aesenc_si128(b2, ek.rk[8]);
	b3 = _mm_aesenc_si128(b3, ek.rk[8]);
	b0 = _mm_aesenc_si128(b0, ek.rk[9]);
	b1 = _mm_aesenc_si128(b1, ek.rk[9]);
	b2 = _mm_aesenc_si128(b2, ek.rk[9]);
	b3 = _mm_aesenc_si128(b3, ek.rk[9]);
	b0 = _mm_aesenc_si128(b0, ek.rk[10]);
	b1 = _mm_aesenc_si128(b1, ek.rk[10]);
	b2 = _mm_aesenc_si128(b2, ek.rk[10]);
	b3 = _mm_aesenc_si128(b3, ek.rk[10]);
	b0 = _mm_aesenc_si128(b0, ek.rk[11]);
	b1 = _mm_aesenc_si128(b1, ek.rk[11]);
	b2 = _mm_aesenc_si128(b2, ek.rk[11]);
	b3 = _mm_aesenc_si128(b3, ek.rk[11]);
	b0 = _mm_aesenc_si128(b0, ek.rk[12]);
	b1 = _mm_aesenc_si128(b1, ek.rk[12]);
	b2 = _mm_aesenc_si128(b2, ek.rk[12]);
	b3 = _mm_aesenc_si128(b3, ek.rk[12]);
	b0 = _mm_aesenc_si128(b0, ek.rk[13]);
	b1 = _mm_aesenc_si128(b1, ek.rk[13]);
	b2 = _mm_aesenc_si128(b2, ek.rk[13]);
	b3 = _mm_aesenc_si128(b3, ek.rk[13]);
	b0 = _mm_aesenclast_si128(b0, ek.rk[14]);
	b1 = _mm_aesenclast_si128(b1, ek.rk[14]);
	b2 = _mm_aesenclast_si128(b2, ek.rk[14]);
	b3 = _mm_aesenclast_si128(b3, ek.rk[14]);
}
// GHASH bulk: Karatsuba lazy reduction — 12 PCLMULQDQ + 1 reduce256 per 4 blocks
// vs the naive 16 PCLMULQDQ + 4 reduce256. h_m = H[63:0] XOR H[127:64] (Karatsuba
// mixed factor, precomputed). Accumulate lo/hi/mid products, reduce once.
static void ghash_update_clmul(
	__m128i &state_br,
	__m128i h_br,
	__m128i h2_br,
	__m128i h3_br,
	__m128i h4_br,
	__m128i h_m,
	__m128i h2_m,
	__m128i h3_m,
	__m128i h4_m,
	unsigned char const *data,
	std::size_t len) {
	std::size_t pos = 0;
	while (pos + 64 <= len) {
		__m128i d0 = byte_bitrev(_mm_loadu_si128(reinterpret_cast<__m128i const *>(data + pos)));
		__m128i d1 = byte_bitrev(_mm_loadu_si128(reinterpret_cast<__m128i const *>(data + pos + 16)));
		__m128i d2 = byte_bitrev(_mm_loadu_si128(reinterpret_cast<__m128i const *>(data + pos + 32)));
		__m128i d3 = byte_bitrev(_mm_loadu_si128(reinterpret_cast<__m128i const *>(data + pos + 48)));
		d0 = _mm_xor_si128(state_br, d0);
		__m128i const d0m = _mm_xor_si128(d0, _mm_srli_si128(d0, 8));
		__m128i const d1m = _mm_xor_si128(d1, _mm_srli_si128(d1, 8));
		__m128i const d2m = _mm_xor_si128(d2, _mm_srli_si128(d2, 8));
		__m128i const d3m = _mm_xor_si128(d3, _mm_srli_si128(d3, 8));
		__m128i acc_lo = _mm_xor_si128(
			_mm_xor_si128(_mm_clmulepi64_si128(d0, h4_br, 0x00), _mm_clmulepi64_si128(d1, h3_br, 0x00)),
			_mm_xor_si128(_mm_clmulepi64_si128(d2, h2_br, 0x00), _mm_clmulepi64_si128(d3, h_br, 0x00)));
		__m128i acc_hi = _mm_xor_si128(
			_mm_xor_si128(_mm_clmulepi64_si128(d0, h4_br, 0x11), _mm_clmulepi64_si128(d1, h3_br, 0x11)),
			_mm_xor_si128(_mm_clmulepi64_si128(d2, h2_br, 0x11), _mm_clmulepi64_si128(d3, h_br, 0x11)));
		__m128i acc_mid = _mm_xor_si128(
			_mm_xor_si128(_mm_clmulepi64_si128(d0m, h4_m, 0x00), _mm_clmulepi64_si128(d1m, h3_m, 0x00)),
			_mm_xor_si128(_mm_clmulepi64_si128(d2m, h2_m, 0x00), _mm_clmulepi64_si128(d3m, h_m, 0x00)));
		acc_mid = _mm_xor_si128(acc_mid, _mm_xor_si128(acc_lo, acc_hi));
		__m128i const t0 = _mm_xor_si128(acc_lo, _mm_slli_si128(acc_mid, 8));
		__m128i const t1 = _mm_xor_si128(acc_hi, _mm_srli_si128(acc_mid, 8));
		state_br = reduce256(t0, t1);
		pos += 64;
	}
	alignas(16) unsigned char block[16];
	while (pos < len) {
		std::size_t const chunk = (len - pos) < 16 ? (len - pos) : 16;
		std::memset(block, 0, 16);
		std::memcpy(block, data + pos, chunk);
		__m128i d = byte_bitrev(_mm_load_si128(reinterpret_cast<__m128i const *>(block)));
		state_br = _mm_xor_si128(state_br, d);
		state_br = gf128_mul(state_br, h_br);
		pos += 16;
	}
}
static void ctr_xor(
	AesniKey256 const &ek,
	__m128i &ctr,
	unsigned char const *in,
	unsigned char *out,
	std::size_t len) {
	std::size_t i = 0;
	for (; i + 64 <= len; i += 64) {
		__m128i b0 = gcm_inc32_fast(ctr);
		__m128i b1 = gcm_inc32_fast(b0);
		__m128i b2 = gcm_inc32_fast(b1);
		__m128i b3 = gcm_inc32_fast(b2);
		ctr = b3;
		aesni_encrypt_4x(ek, b0, b1, b2, b3);
		auto const *src = reinterpret_cast<__m128i const *>(in + i);
		auto *dst = reinterpret_cast<__m128i *>(out + i);
		_mm_storeu_si128(dst, _mm_xor_si128(b0, _mm_loadu_si128(src)));
		_mm_storeu_si128(dst + 1, _mm_xor_si128(b1, _mm_loadu_si128(src + 1)));
		_mm_storeu_si128(dst + 2, _mm_xor_si128(b2, _mm_loadu_si128(src + 2)));
		_mm_storeu_si128(dst + 3, _mm_xor_si128(b3, _mm_loadu_si128(src + 3)));
	}
	for (; i < len; i += 16) {
		ctr = gcm_inc32_fast(ctr);
		__m128i ks = aesni_encrypt_block(ek, ctr);
		std::size_t const chunk = (len - i) < 16 ? (len - i) : 16;
		if (chunk == 16) {
			_mm_storeu_si128(
				reinterpret_cast<__m128i *>(out + i),
				_mm_xor_si128(ks, _mm_loadu_si128(reinterpret_cast<__m128i const *>(in + i))));
		} else {
			alignas(16) unsigned char tmp[16]{};
			std::memcpy(tmp, in + i, chunk);
			__m128i t = _mm_xor_si128(ks, _mm_load_si128(reinterpret_cast<__m128i const *>(tmp)));
			_mm_store_si128(reinterpret_cast<__m128i *>(tmp), t);
			std::memcpy(out + i, tmp, chunk);
		}
	}
}
extern "C" {
int conflux_aes_gcm_encrypt_aesni(
	unsigned char const *key,
	unsigned char const *iv,
	unsigned char const *pt,
	std::size_t pt_len,
	unsigned char const *aad,
	std::size_t aad_len,
	unsigned char *out) {
	auto const &kc = get_key_ctx(key);

	alignas(16) unsigned char j0_buf[16]{};
	std::memcpy(j0_buf, iv, 12);
	j0_buf[15] = 1;
	__m128i const j0 = _mm_load_si128(reinterpret_cast<__m128i const *>(j0_buf));

	__m128i ctr = j0;
	ctr_xor(kc.ek, ctr, pt, out, pt_len);

	__m128i ghash_br = _mm_setzero_si128();
	if (aad_len > 0) {
		ghash_update_clmul(
			ghash_br,
			kc.h_br,
			kc.h2_br,
			kc.h3_br,
			kc.h4_br,
			kc.h_m,
			kc.h2_m,
			kc.h3_m,
			kc.h4_m,
			aad,
			aad_len);
	}
	ghash_update_clmul(ghash_br, kc.h_br, kc.h2_br, kc.h3_br, kc.h4_br, kc.h_m, kc.h2_m, kc.h3_m, kc.h4_m, out, pt_len);

	alignas(16) unsigned char len_block[16]{};
	uint64_t const aad_bits = aad_len * 8;
	uint64_t const ct_bits = pt_len * 8;
	for (int i = 0; i < 8; ++i) {
		len_block[i] = static_cast<unsigned char>(aad_bits >> (56 - i * 8));
		len_block[i + 8] = static_cast<unsigned char>(ct_bits >> (56 - i * 8));
	}
	__m128i lb = _mm_load_si128(reinterpret_cast<__m128i const *>(len_block));
	lb = byte_bitrev(lb);
	ghash_br = _mm_xor_si128(ghash_br, lb);
	ghash_br = gf128_mul(ghash_br, kc.h_br);

	__m128i const ghash_state = byte_bitrev(ghash_br);

	__m128i const j0_enc = aesni_encrypt_block(kc.ek, j0);
	__m128i const tag = _mm_xor_si128(j0_enc, ghash_state);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(out + pt_len), tag);

	return 0;
}
int conflux_aes_gcm_decrypt_aesni(
	unsigned char const *key,
	unsigned char const *iv,
	unsigned char const *ct_tag,
	std::size_t ct_tag_len,
	unsigned char const *aad,
	std::size_t aad_len,
	unsigned char *out) {
	if (ct_tag_len < 16) {
		return -1;
	}
	std::size_t const ct_len = ct_tag_len - 16;

	auto const &kc = get_key_ctx(key);

	alignas(16) unsigned char j0_buf[16]{};
	std::memcpy(j0_buf, iv, 12);
	j0_buf[15] = 1;
	__m128i const j0 = _mm_load_si128(reinterpret_cast<__m128i const *>(j0_buf));

	__m128i ghash_br = _mm_setzero_si128();
	if (aad_len > 0) {
		ghash_update_clmul(
			ghash_br,
			kc.h_br,
			kc.h2_br,
			kc.h3_br,
			kc.h4_br,
			kc.h_m,
			kc.h2_m,
			kc.h3_m,
			kc.h4_m,
			aad,
			aad_len);
	}
	ghash_update_clmul(
		ghash_br,
		kc.h_br,
		kc.h2_br,
		kc.h3_br,
		kc.h4_br,
		kc.h_m,
		kc.h2_m,
		kc.h3_m,
		kc.h4_m,
		ct_tag,
		ct_len);

	alignas(16) unsigned char len_block[16]{};
	uint64_t const aad_bits = aad_len * 8;
	uint64_t const ct_bits = ct_len * 8;
	for (int i = 0; i < 8; ++i) {
		len_block[i] = static_cast<unsigned char>(aad_bits >> (56 - i * 8));
		len_block[i + 8] = static_cast<unsigned char>(ct_bits >> (56 - i * 8));
	}
	__m128i lb = _mm_load_si128(reinterpret_cast<__m128i const *>(len_block));
	lb = byte_bitrev(lb);
	ghash_br = _mm_xor_si128(ghash_br, lb);
	ghash_br = gf128_mul(ghash_br, kc.h_br);

	__m128i const ghash_state = byte_bitrev(ghash_br);
	__m128i const j0_enc = aesni_encrypt_block(kc.ek, j0);
	__m128i const expected_tag = _mm_xor_si128(j0_enc, ghash_state);

	// Constant-time tag comparison
	__m128i const claimed = _mm_loadu_si128(reinterpret_cast<__m128i const *>(ct_tag + ct_len));
	__m128i const diff = _mm_xor_si128(expected_tag, claimed);
	if (!_mm_test_all_zeros(diff, diff)) {
		return -1;
	}

	__m128i ctr = j0;
	ctr_xor(kc.ek, ctr, ct_tag, out, ct_len);

	return 0;
}
void conflux_hex_encode_ssse3(
	unsigned char const *in,
	std::size_t len,
	char *out) {
	__m128i const hex_lut =
		_mm_setr_epi8('0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
	__m128i const mask_lo = _mm_set1_epi8(0x0F);

	std::size_t i = 0;
	for (; i + 16 <= len; i += 16) {
		__m128i const v = _mm_loadu_si128(reinterpret_cast<__m128i const *>(in + i));
		__m128i const hi = _mm_and_si128(_mm_srli_epi16(v, 4), mask_lo);
		__m128i const lo = _mm_and_si128(v, mask_lo);
		__m128i const hex_hi = _mm_shuffle_epi8(hex_lut, hi);
		__m128i const hex_lo = _mm_shuffle_epi8(hex_lut, lo);
		__m128i const r0 = _mm_unpacklo_epi8(hex_hi, hex_lo);
		__m128i const r1 = _mm_unpackhi_epi8(hex_hi, hex_lo);
		_mm_storeu_si128(reinterpret_cast<__m128i *>(out + i * 2), r0);
		_mm_storeu_si128(reinterpret_cast<__m128i *>(out + i * 2 + 16), r1);
	}

	static constexpr char kHex[] = "0123456789abcdef";
	for (; i < len; ++i) {
		out[i * 2] = kHex[in[i] >> 4U];
		out[i * 2 + 1] = kHex[in[i] & 0x0FU];
	}
}
} // extern "C"
