#include <cstddef>
#include <cstdint>
#include <cstring>
#include <smmintrin.h>
#include <tmmintrin.h>
#include <wmmintrin.h>
import conflux.types;
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
// GCM ↔ PCLMULQDQ domain: reverse bits within each byte (no byte-swap)
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
// PCLMULQDQ multiply + reduce mod P(x) = x^128+x^7+x^2+x+1
// Inputs/outputs in byte-bitrev'd domain. Folds hi 128 bits into lo.
inline static __m128i gf128_mul(
	__m128i a,
	__m128i b) {
	__m128i t0 = _mm_clmulepi64_si128(a, b, 0x00);
	__m128i t1 = _mm_clmulepi64_si128(a, b, 0x11);
	__m128i const t2 = _mm_xor_si128(_mm_clmulepi64_si128(a, b, 0x01), _mm_clmulepi64_si128(a, b, 0x10));
	t0 = _mm_xor_si128(t0, _mm_slli_si128(t2, 8));
	t1 = _mm_xor_si128(t1, _mm_srli_si128(t2, 8));

	// Full 128-bit left shifts of hi
	__m128i const hi1 = _mm_or_si128(_mm_slli_epi64(t1, 1), _mm_slli_si128(_mm_srli_epi64(t1, 63), 8));
	__m128i const hi2 = _mm_or_si128(_mm_slli_epi64(t1, 2), _mm_slli_si128(_mm_srli_epi64(t1, 62), 8));
	__m128i const hi7 = _mm_or_si128(_mm_slli_epi64(t1, 7), _mm_slli_si128(_mm_srli_epi64(t1, 57), 8));

	__m128i r = _mm_xor_si128(t0, t1);
	r = _mm_xor_si128(r, hi1);
	r = _mm_xor_si128(r, hi2);
	r = _mm_xor_si128(r, hi7);

	// Overflow from hi<<1 (1 bit), hi<<2 (2 bits), hi<<7 (7 bits)
	__m128i const hi_hi = _mm_srli_si128(t1, 8);
	__m128i const ov =
		_mm_xor_si128(_mm_srli_epi64(hi_hi, 63), _mm_xor_si128(_mm_srli_epi64(hi_hi, 62), _mm_srli_epi64(hi_hi, 57)));
	r = _mm_xor_si128(r, ov);
	r = _mm_xor_si128(
		r,
		_mm_xor_si128(_mm_slli_epi64(ov, 1), _mm_xor_si128(_mm_slli_epi64(ov, 2), _mm_slli_epi64(ov, 7))));
	return r;
}
inline static __m128i gcm_inc32_sse(
	__m128i ctr) {
	alignas(16) unsigned char buf[16];
	_mm_store_si128(reinterpret_cast<__m128i *>(buf), ctr);
	for (int i = 15; i >= 12; --i) {
		if (++buf[i] != 0) {
			break;
		}
	}
	return _mm_load_si128(reinterpret_cast<__m128i const *>(buf));
}
// GHASH: state kept in byte_bitrev'd domain. h_br = byte_bitrev(H).
static void ghash_update_clmul(
	__m128i &state_br,
	__m128i h_br,
	unsigned char const *data,
	std::size_t len) {
	alignas(16) unsigned char block[16];
	std::size_t pos = 0;
	while (pos < len) {
		std::size_t const chunk = (len - pos) < 16 ? (len - pos) : 16;
		std::memset(block, 0, 16);
		std::memcpy(block, data + pos, chunk);
		__m128i d = _mm_load_si128(reinterpret_cast<__m128i const *>(block));
		d = byte_bitrev(d);
		state_br = _mm_xor_si128(state_br, d);
		state_br = gf128_mul(state_br, h_br);
		pos += 16;
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
	auto const ek = aesni_expand_key(key);

	__m128i const h = aesni_encrypt_block(ek, _mm_setzero_si128());
	__m128i const h_br = byte_bitrev(h);

	alignas(16) unsigned char j0_buf[16]{};
	std::memcpy(j0_buf, iv, 12);
	j0_buf[15] = 1;
	__m128i const j0 = _mm_load_si128(reinterpret_cast<__m128i const *>(j0_buf));

	__m128i ctr = j0;
	for (std::size_t i = 0; i < pt_len; i += 16) {
		ctr = gcm_inc32_sse(ctr);
		__m128i const keystream = aesni_encrypt_block(ek, ctr);
		std::size_t const chunk = (pt_len - i) < 16 ? (pt_len - i) : 16;
		alignas(16) unsigned char ks[16];
		_mm_store_si128(reinterpret_cast<__m128i *>(ks), keystream);
		for (std::size_t j = 0; j < chunk; ++j) {
			out[i + j] = static_cast<unsigned char>(pt[i + j] ^ ks[j]);
		}
	}

	__m128i ghash_br = _mm_setzero_si128();
	if (aad_len > 0) {
		ghash_update_clmul(ghash_br, h_br, aad, aad_len);
	}
	ghash_update_clmul(ghash_br, h_br, out, pt_len);

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
	ghash_br = gf128_mul(ghash_br, h_br);

	__m128i const ghash_state = byte_bitrev(ghash_br);

	__m128i const j0_enc = aesni_encrypt_block(ek, j0);
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

	auto const ek = aesni_expand_key(key);

	__m128i const h = aesni_encrypt_block(ek, _mm_setzero_si128());
	__m128i const h_br = byte_bitrev(h);

	alignas(16) unsigned char j0_buf[16]{};
	std::memcpy(j0_buf, iv, 12);
	j0_buf[15] = 1;
	__m128i const j0 = _mm_load_si128(reinterpret_cast<__m128i const *>(j0_buf));

	__m128i ghash_br = _mm_setzero_si128();
	if (aad_len > 0) {
		ghash_update_clmul(ghash_br, h_br, aad, aad_len);
	}
	ghash_update_clmul(ghash_br, h_br, ct_tag, ct_len);

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
	ghash_br = gf128_mul(ghash_br, h_br);

	__m128i const ghash_state = byte_bitrev(ghash_br);
	__m128i const j0_enc = aesni_encrypt_block(ek, j0);
	__m128i const expected_tag = _mm_xor_si128(j0_enc, ghash_state);

	// Constant-time tag comparison
	__m128i const claimed = _mm_loadu_si128(reinterpret_cast<__m128i const *>(ct_tag + ct_len));
	__m128i const diff = _mm_xor_si128(expected_tag, claimed);
	if (!_mm_test_all_zeros(diff, diff)) {
		return -1;
	}

	// Decrypt CTR
	__m128i ctr = j0;
	for (std::size_t i = 0; i < ct_len; i += 16) {
		ctr = gcm_inc32_sse(ctr);
		__m128i const keystream = aesni_encrypt_block(ek, ctr);
		std::size_t const chunk = (ct_len - i) < 16 ? (ct_len - i) : 16;
		alignas(16) unsigned char ks[16];
		_mm_store_si128(reinterpret_cast<__m128i *>(ks), keystream);
		for (std::size_t j = 0; j < chunk; ++j) {
			out[i + j] = static_cast<unsigned char>(ct_tag[i + j] ^ ks[j]);
		}
	}

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
