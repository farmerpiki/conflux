module;
#if defined(CONFLUX_CRYPTO_USE_AESNI)
extern "C" {
int conflux_aes_gcm_encrypt_aesni(
	unsigned char const *key,
	unsigned char const *iv,
	unsigned char const *pt,
	__SIZE_TYPE__ pt_len,
	unsigned char const *aad,
	__SIZE_TYPE__ aad_len,
	unsigned char *out);
int conflux_aes_gcm_decrypt_aesni(
	unsigned char const *key,
	unsigned char const *iv,
	unsigned char const *ct_tag,
	__SIZE_TYPE__ ct_tag_len,
	unsigned char const *aad,
	__SIZE_TYPE__ aad_len,
	unsigned char *out);
}
#endif
#include "cpu_features.hxx"
#include "simd_backend.hxx"

export module conflux.crypto;
import std;
import conflux.types;
import std.compat;
namespace {

constexpr std::string_view kB64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kB64UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::array<std::int8_t, 256> make_b64_table(
	std::string_view alphabet) {
	std::array<std::int8_t, 256> t{};
	t.fill(-1);
	for (std::size_t i = 0; i < 64; ++i) {
		t[static_cast<unsigned char>(alphabet[i])] = static_cast<std::int8_t>(i);
	}
	return t;
}
constexpr auto kB64Table = make_b64_table(kB64Alphabet);
constexpr auto kB64UrlTable = make_b64_table(kB64UrlAlphabet);
std::string b64_encode_impl(
	std::span<unsigned char const> in,
	std::string_view alphabet,
	bool padding) {
	std::string out;
	out.reserve(((in.size() + 2) / 3) * 4);
	for (std::size_t i = 0; i < in.size(); i += 3) {
		unsigned int v = static_cast<unsigned int>(in[i]) << 16U;
		if (i + 1 < in.size()) {
			v |= static_cast<unsigned int>(in[i + 1]) << 8U;
		}
		if (i + 2 < in.size()) {
			v |= static_cast<unsigned int>(in[i + 2]);
		}
		out += alphabet[(v >> 18U) & 0x3FU];
		out += alphabet[(v >> 12U) & 0x3FU];
		if (i + 1 < in.size()) {
			out += alphabet[(v >> 6U) & 0x3FU];
		} else if (padding) {
			out += '=';
		}
		if (i + 2 < in.size()) {
			out += alphabet[v & 0x3FU];
		} else if (padding) {
			out += '=';
		}
	}
	return out;
}
std::string b64_decode_impl(
	std::string_view encoded,
	std::span<std::int8_t const, 256> table) {
	if (encoded.size() % 4U == 1U) {
		return {};
	}
	auto const pad_pos = encoded.find('=');
	std::size_t const payload_size = pad_pos == std::string_view::npos ? encoded.size() : pad_pos;
	if (pad_pos != std::string_view::npos) {
		std::size_t const padding = encoded.size() - pad_pos;
		if (padding > 2U || encoded.size() % 4U != 0U) {
			return {};
		}
		for (std::size_t i = pad_pos; i < encoded.size(); ++i) {
			if (encoded[i] != '=') {
				return {};
			}
		}
	}
	if (payload_size % 4U == 1U) {
		return {};
	}
	std::string out;
	out.reserve(((payload_size * 3) / 4) + 1);
	int bits = 0;
	std::uint32_t val = 0;
	for (char const raw: encoded.substr(0, payload_size)) {
		auto c = static_cast<unsigned char>(raw);
		std::int8_t const d = table[c];
		if (d < 0) {
			return {};
		}
		val = (val << 6U) | static_cast<std::uint32_t>(d);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += static_cast<char>((val >> static_cast<unsigned>(bits)) & 0xFFU);
		}
	}
	if (bits > 0 && (val & ((std::uint32_t{1} << static_cast<unsigned>(bits)) - 1U)) != 0U) {
		return {};
	}
	return out;
}

} // namespace

export namespace conflux::crypto {

[[gnu::always_inline]] inline std::span<unsigned char const> to_unsigned_span(
	std::string_view s) noexcept {
	return {
		reinterpret_cast<unsigned char const *>(s.data()),
		s.size()}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}
std::string base64_encode(
	std::span<unsigned char const> in) {
	return b64_encode_impl(in, kB64Alphabet, true);
}
std::string base64_decode(
	std::string_view encoded) {
	return b64_decode_impl(encoded, kB64Table);
}
std::string base64url_encode(
	std::span<unsigned char const> in) {
	return b64_encode_impl(in, kB64UrlAlphabet, false);
}
std::string base64url_decode(
	std::string_view encoded) {
	return b64_decode_impl(encoded, kB64UrlTable);
}

} // namespace conflux::crypto

namespace conflux::crypto::crypto_detail {

struct Sha256State {
	std::array<std::uint32_t, 8> h{};
	std::array<unsigned char, 64> pending{};
	std::uint64_t bytes{};
	std::size_t pending_size{};
};

} // namespace conflux::crypto::crypto_detail

export namespace conflux::crypto {

struct HmacSha256Key {
	std::array<unsigned char, 64> inner{};
	std::array<unsigned char, 64> outer{};
};

} // namespace conflux::crypto

namespace conflux::crypto::crypto_detail {

[[nodiscard]] std::uint32_t rotr32(
	std::uint32_t v,
	std::uint32_t n) noexcept {
	return (v >> n) | (v << (32U - n));
}

void sha256_compress(
	std::array<std::uint32_t, 8> &h,
	unsigned char const *block) noexcept {
	static constexpr std::array<std::uint32_t, 64> K{
		0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
		0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
		0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
		0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
		0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
		0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
		0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
		0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
	};

	std::array<std::uint32_t, 64> w{};
	for (std::size_t i = 0; i < 16; ++i) {
		std::size_t const off = i * 4U;
		w[i] = (static_cast<std::uint32_t>(block[off]) << 24U)
			 | (static_cast<std::uint32_t>(block[off + 1U]) << 16U)
			 | (static_cast<std::uint32_t>(block[off + 2U]) << 8U)
			 | static_cast<std::uint32_t>(block[off + 3U]);
	}
	for (std::size_t i = 16; i < 64; ++i) {
		std::uint32_t const s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
		std::uint32_t const s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
		w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
	}

	auto [a, b, c, d, e, f, g, hh] = h;
	for (std::size_t i = 0; i < 64; ++i) {
		std::uint32_t const ch = (e & f) ^ (~e & g);
		std::uint32_t const maj = (a & b) ^ (a & c) ^ (b & c);
		std::uint32_t const s1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
		std::uint32_t const s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
		std::uint32_t const t1 = hh + s1 + ch + K[i] + w[i];
		std::uint32_t const t2 = s0 + maj;
		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hh;
}

[[nodiscard]] Sha256State sha256_init() noexcept {
	return Sha256State{
		.h = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U}
    };
}

void sha256_update(
	Sha256State &state,
	std::span<unsigned char const> msg) noexcept {
	state.bytes += msg.size();
	if (state.pending_size != 0U) {
		std::size_t const n = std::min(msg.size(), state.pending.size() - state.pending_size);
		std::ranges::copy(msg.first(n), state.pending.begin() + static_cast<std::ptrdiff_t>(state.pending_size));
		state.pending_size += n;
		msg = msg.subspan(n);
		if (state.pending_size == state.pending.size()) {
			sha256_compress(state.h, state.pending.data());
			state.pending_size = 0U;
		}
	}
	while (msg.size() >= 64U) {
		sha256_compress(state.h, msg.data());
		msg = msg.subspan(64U);
	}
	if (!msg.empty()) {
		std::ranges::copy(msg, state.pending.begin());
		state.pending_size = msg.size();
	}
}

[[nodiscard]] std::array<unsigned char, 32> sha256_final(
	Sha256State state) noexcept {
	std::uint64_t const bit_len = state.bytes * 8ULL;
	state.pending[state.pending_size++] = 0x80U;
	if (state.pending_size > 56U) {
		for (std::size_t i = state.pending_size; i < 64U; ++i) {
			state.pending[i] = 0U;
		}
		sha256_compress(state.h, state.pending.data());
		state.pending_size = 0U;
	}
	for (std::size_t i = state.pending_size; i < 56U; ++i) {
		state.pending[i] = 0U;
	}
	for (std::size_t i = 0; i < 8U; ++i) {
		state.pending[56U + i] = static_cast<unsigned char>((bit_len >> (56U - (i * 8U))) & 0xFFU);
	}
	sha256_compress(state.h, state.pending.data());
	std::array<unsigned char, 32> out{};
	for (std::size_t i = 0; i < 32U; ++i) {
		out[i] = static_cast<unsigned char>((state.h[i / 4U] >> (24U - ((i % 4U) * 8U))) & 0xFFU);
	}
	return out;
}

[[nodiscard]] std::array<unsigned char, 32> sha256_noalloc(
	std::span<unsigned char const> msg) noexcept {
	auto state = sha256_init();
	sha256_update(state, msg);
	return sha256_final(state);
}

} // namespace conflux::crypto::crypto_detail

export namespace conflux::crypto {

[[nodiscard]] HmacSha256Key hmac_sha256_key(
	std::span<unsigned char const> key) noexcept {
	std::array<unsigned char, 64> kpad{};
	if (key.size() > 64U) {
		auto hashed = crypto_detail::sha256_noalloc(key);
		std::ranges::copy(hashed, kpad.begin());
	} else {
		std::ranges::copy(key, kpad.begin());
	}
	HmacSha256Key out{};
	for (std::size_t i = 0; i < kpad.size(); ++i) {
		out.inner[i] = static_cast<unsigned char>(kpad[i] ^ 0x36U);
		out.outer[i] = static_cast<unsigned char>(kpad[i] ^ 0x5CU);
	}
	return out;
}

[[nodiscard]] std::array<unsigned char, 32> hmac_sha256_precomputed(
	HmacSha256Key const &key,
	std::span<unsigned char const> a,
	std::span<unsigned char const> b = {}) noexcept {
	auto inner = crypto_detail::sha256_init();
	crypto_detail::sha256_update(inner, key.inner);
	crypto_detail::sha256_update(inner, a);
	crypto_detail::sha256_update(inner, b);
	auto inner_digest = crypto_detail::sha256_final(inner);
	auto outer = crypto_detail::sha256_init();
	crypto_detail::sha256_update(outer, key.outer);
	crypto_detail::sha256_update(outer, inner_digest);
	return crypto_detail::sha256_final(outer);
}

} // namespace conflux::crypto
// ---------------------------------------------------------------------------
// SHA padding helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<unsigned char> make_sha_padded(
	std::span<unsigned char const> msg) {
	std::size_t const with_marker = msg.size() + 1;
	std::size_t const zero_pad = (56 + 64 - (with_marker % 64)) % 64;
	std::size_t const total = with_marker + zero_pad + 8;
	std::vector<unsigned char> padded(total);
	for (std::size_t i = 0; i < msg.size(); ++i) {
		padded[i] = msg[i];
	}
	padded[msg.size()] = 0x80U;
	std::uint64_t const bit_len = msg.size() * 8ULL;
	std::size_t const len_pos = with_marker + zero_pad;
	for (int s = 56; s >= 0; s -= 8) {
		padded[len_pos + static_cast<std::size_t>((56 - s) / 8)] = static_cast<unsigned char>((bit_len >> s) & 0xFFU);
	}
	return padded;
}

// ---------------------------------------------------------------------------
// SHA-1 (FIPS 180-4)
// ---------------------------------------------------------------------------

export namespace conflux::crypto {

std::array<unsigned char, 20> sha1(
	std::span<unsigned char const> msg) {
	std::array<std::uint32_t, 5> h{0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};

	std::vector<unsigned char> padded = make_sha_padded(msg);

	auto rot32 = [](std::uint32_t v, unsigned n) -> std::uint32_t { return (v << n) | (v >> (32 - n)); };

	for (std::size_t blk = 0; blk < padded.size(); blk += 64) {
		std::array<std::uint32_t, 80> w{};
		for (int i = 0; i < 16; ++i) {
			auto b = std::span{padded}.subspan(blk + (static_cast<std::size_t>(i) * 4), 4);
			w[static_cast<std::size_t>(i)] = (static_cast<std::uint32_t>(b[0]) << 24)
										   | (static_cast<std::uint32_t>(b[1]) << 16)
										   | (static_cast<std::uint32_t>(b[2]) << 8)
										   | static_cast<std::uint32_t>(b[3]);
		}
		for (std::size_t i = 16; i < 80; ++i) {
			w[i] = rot32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}
		auto [a, b, c, d, e] = h;
		for (std::size_t i = 0; i < 80; ++i) {
			std::uint32_t f{};
			std::uint32_t k{};
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5A827999U;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1U;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDCU;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6U;
			}
			std::uint32_t const tmp = rot32(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rot32(b, 30);
			b = a;
			a = tmp;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}

	std::array<unsigned char, 20> out{};
	for (std::size_t i = 0; i < 5; ++i) {
		out[(i * 4) + 0] = static_cast<unsigned char>(h[i] >> 24);
		out[(i * 4) + 1] = static_cast<unsigned char>(h[i] >> 16);
		out[(i * 4) + 2] = static_cast<unsigned char>(h[i] >> 8);
		out[(i * 4) + 3] = static_cast<unsigned char>(h[i]);
	}
	return out;
}
// ---------------------------------------------------------------------------
// HMAC-SHA1 (RFC 2104)
// ---------------------------------------------------------------------------

std::array<unsigned char, 20> hmac_sha1(
	std::span<unsigned char const> key,
	std::span<unsigned char const> msg) {
	std::array<unsigned char, 64> k_pad{};
	if (key.size() > 64) {
		auto kh = sha1(key);
		std::ranges::copy(kh, k_pad.begin());
	} else {
		std::ranges::copy(key, k_pad.begin());
	}

	std::vector<unsigned char> inner_buf(64 + msg.size());
	for (std::size_t i = 0; i < 64; ++i) {
		inner_buf[i] = static_cast<unsigned char>(k_pad[i] ^ 0x36U);
	}
	std::ranges::copy(msg, inner_buf.begin() + 64);
	auto inner = sha1(inner_buf);

	std::array<unsigned char, 84> outer_buf{};
	for (std::size_t i = 0; i < 64; ++i) {
		outer_buf[i] = static_cast<unsigned char>(k_pad[i] ^ 0x5CU);
	}
	std::ranges::copy(inner, outer_buf.begin() + 64);
	return sha1(std::span{outer_buf.data(), 84});
}
// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

std::array<unsigned char, 32> sha256(
	std::span<unsigned char const> msg) {
	return conflux::crypto::crypto_detail::sha256_noalloc(msg);
}
// ---------------------------------------------------------------------------
// HMAC-SHA256 (RFC 2104)
// ---------------------------------------------------------------------------
std::array<unsigned char, 32> hmac_sha256(
	std::span<unsigned char const> key,
	std::span<unsigned char const> msg) {
	return conflux::crypto::hmac_sha256_precomputed(conflux::crypto::hmac_sha256_key(key), msg);
}
bool constant_time_eq(
	std::string_view a,
	std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
#if defined(CONFLUX_STDSIMD) && CONFLUX_SIMD_SELECTION_DIRECT
	constexpr std::size_t kStdsimdThreshold = 64;
	if (a.size() >= kStdsimdThreshold) {
		return conflux_constant_time_eq_stdsimd(
				   reinterpret_cast<unsigned char const *>(a.data()),
				   reinterpret_cast<unsigned char const *>(b.data()),
				   a.size())
			!= 0;
	}
#elif defined(CONFLUX_STDSIMD) && CONFLUX_SIMD_SELECTION_RUNTIME
	constexpr std::size_t kStdsimdThreshold = 64;
	if (a.size() >= kStdsimdThreshold && conflux_cpu_supports_avx2()) {
		return conflux_constant_time_eq_stdsimd(
				   reinterpret_cast<unsigned char const *>(a.data()),
				   reinterpret_cast<unsigned char const *>(b.data()),
				   a.size())
			!= 0;
	}
#endif
	unsigned char acc = 0;
	for (std::size_t i = 0; i < a.size(); ++i) {
		acc = static_cast<unsigned char>(acc | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
	}
	return acc == 0;
}

} // namespace conflux::crypto
// ---------------------------------------------------------------------------
// AES-256-GCM (NIST SP 800-38D)
// ---------------------------------------------------------------------------

namespace {

#if !defined(CONFLUX_CRYPTO_USE_AESNI) || CONFLUX_CPU_FEATURE_PROBES_RUNTIME

constexpr std::array<unsigned char, 256> kAesSbox{
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
	0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
	0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
	0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
	0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
	0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
	0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
	0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
	0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
	0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
	0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
	0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
	0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
	0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

constexpr std::array<unsigned char, 11> kAesRcon{0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
constexpr std::uint32_t aes_word(
	unsigned char a,
	unsigned char b,
	unsigned char c,
	unsigned char d) {
	return (static_cast<std::uint32_t>(a) << 24)
		 | (static_cast<std::uint32_t>(b) << 16)
		 | (static_cast<std::uint32_t>(c) << 8)
		 | d;
}
constexpr std::uint32_t aes_sub_word(
	std::uint32_t w) {
	return aes_word(
		kAesSbox[(w >> 24) & 0xFF],
		kAesSbox[(w >> 16) & 0xFF],
		kAesSbox[(w >> 8) & 0xFF],
		kAesSbox[w & 0xFF]);
}
constexpr std::uint32_t aes_rot_word(
	std::uint32_t w) {
	return (w << 8) | (w >> 24);
}
	#if !defined(CONFLUX_CRYPTO_USE_AESNI) || CONFLUX_CPU_FEATURE_PROBES_RUNTIME
struct AesKey256 {
	std::array<std::uint32_t, 60> rk{};
};
AesKey256 aes256_expand_key(
	std::span<unsigned char const> key) {
	AesKey256 ek{};
	for (std::size_t i = 0; i < 8; ++i) {
		ek.rk[i] = aes_word(key[i * 4], key[i * 4 + 1], key[i * 4 + 2], key[i * 4 + 3]);
	}
	for (std::size_t i = 8; i < 60; ++i) {
		std::uint32_t t = ek.rk[i - 1];
		if (i % 8 == 0) {
			t = aes_sub_word(aes_rot_word(t)) ^ (static_cast<std::uint32_t>(kAesRcon[i / 8]) << 24);
		} else if (i % 8 == 4) {
			t = aes_sub_word(t);
		}
		ek.rk[i] = ek.rk[i - 8] ^ t;
	}
	return ek;
}
constexpr unsigned char xtime(
	unsigned char x) {
	return static_cast<unsigned char>((x << 1) ^ ((x >> 7) * 0x1b));
}
void aes256_encrypt_block(
	AesKey256 const &ek,
	unsigned char const in[16],
	unsigned char out[16]) {
	std::array<unsigned char, 16> s{};
	for (std::size_t i = 0; i < 16; ++i) {
		s[i] = static_cast<unsigned char>(in[i] ^ static_cast<unsigned char>(ek.rk[i / 4] >> (24 - (i % 4) * 8)));
	}

	for (int round = 1; round <= 14; ++round) {
		std::array<unsigned char, 16> t{};
		for (std::size_t i = 0; i < 16; ++i) {
			t[i] = kAesSbox[s[i]];
		}
		// ShiftRows
		std::array<unsigned char, 16> sr{};
		for (std::size_t col = 0; col < 4; ++col) {
			sr[col * 4 + 0] = t[col * 4 + 0];
			sr[col * 4 + 1] = t[((col + 1) % 4) * 4 + 1];
			sr[col * 4 + 2] = t[((col + 2) % 4) * 4 + 2];
			sr[col * 4 + 3] = t[((col + 3) % 4) * 4 + 3];
		}
		if (round < 14) {
			// MixColumns
			for (std::size_t col = 0; col < 4; ++col) {
				auto a0 = sr[col * 4 + 0], a1 = sr[col * 4 + 1], a2 = sr[col * 4 + 2], a3 = sr[col * 4 + 3];
				auto x0 = xtime(a0), x1 = xtime(a1), x2 = xtime(a2), x3 = xtime(a3);
				s[col * 4 + 0] = static_cast<unsigned char>(x0 ^ x1 ^ a1 ^ a2 ^ a3);
				s[col * 4 + 1] = static_cast<unsigned char>(a0 ^ x1 ^ x2 ^ a2 ^ a3);
				s[col * 4 + 2] = static_cast<unsigned char>(a0 ^ a1 ^ x2 ^ x3 ^ a3);
				s[col * 4 + 3] = static_cast<unsigned char>(x0 ^ a0 ^ a1 ^ a2 ^ x3);
			}
		} else {
			std::ranges::copy(sr, s.begin());
		}
		// AddRoundKey
		for (std::size_t i = 0; i < 16; ++i) {
			s[i] = static_cast<unsigned char>(
				s[i]
				^ static_cast<unsigned char>(ek.rk[static_cast<std::size_t>(round) * 4 + i / 4] >> (24 - (i % 4) * 8)));
		}
	}
	std::ranges::copy(s, out);
}
// GCM: GF(2^128) multiplication (bit-by-bit, constant-time for equal-length inputs)
void ghash_mult(
	unsigned char result[16],
	unsigned char const h[16],
	unsigned char const x[16]) {
	std::array<unsigned char, 16> v{};
	std::ranges::copy(std::span{h, 16}, v.begin());
	std::array<unsigned char, 16> z{};

	for (int i = 0; i < 128; ++i) {
		if (((x[i / 8] >> (7 - (i % 8))) & 1) != 0) {
			for (std::size_t j = 0; j < 16; ++j) {
				z[j] = static_cast<unsigned char>(z[j] ^ v[j]);
			}
		}
		unsigned char const carry = v[15] & 1;
		for (int j = 15; j > 0; --j) {
			v[static_cast<std::size_t>(j)] = static_cast<unsigned char>(
				(v[static_cast<std::size_t>(j)] >> 1) | (v[static_cast<std::size_t>(j) - 1] << 7));
		}
		v[0] >>= 1;
		if (carry != 0u) {
			v[0] = static_cast<unsigned char>(v[0] ^ 0xe1);
		}
	}
	std::ranges::copy(z, result);
}
void ghash_update(
	unsigned char state[16],
	unsigned char const h[16],
	std::span<unsigned char const> data) {
	std::array<unsigned char, 16> block{};
	std::size_t pos = 0;
	while (pos < data.size()) {
		std::size_t const chunk = std::min(std::size_t{16}, data.size() - pos);
		block.fill(0);
		std::ranges::copy(data.subspan(pos, chunk), block.begin());
		for (std::size_t i = 0; i < 16; ++i) {
			state[i] = static_cast<unsigned char>(state[i] ^ block[i]);
		}
		ghash_mult(state, h, state);
		pos += 16;
	}
}
void gcm_inc32(
	unsigned char ctr[16]) {
	for (int i = 15; i >= 12; --i) {
		if (++ctr[i] != 0) {
			break;
		}
	}
}
	#endif

#endif

} // namespace

export namespace conflux::crypto {

std::expected<std::vector<unsigned char>, std::string> aes_gcm_encrypt(
	std::span<unsigned char const> key,
	std::span<unsigned char const> iv,
	std::span<unsigned char const> plaintext,
	std::span<unsigned char const> aad) {
	if (key.size() != 32) {
		return std::unexpected(std::string{"aes_gcm_encrypt: key must be 32 bytes"});
	}
	if (iv.size() != 12) {
		return std::unexpected(std::string{"aes_gcm_encrypt: iv must be 12 bytes"});
	}

#if defined(CONFLUX_CRYPTO_USE_AESNI)
	#if CONFLUX_CPU_FEATURE_PROBES_RUNTIME
	if (conflux_cpu_supports_aesni_pclmul_sse41())
	#endif
	{
		std::vector<unsigned char> out(plaintext.size() + 16);
		conflux_aes_gcm_encrypt_aesni(
			key.data(),
			iv.data(),
			plaintext.data(),
			plaintext.size(),
			aad.data(),
			aad.size(),
			out.data());
		return out;
	}
#endif

#if !defined(CONFLUX_CRYPTO_USE_AESNI) || CONFLUX_CPU_FEATURE_PROBES_RUNTIME
	auto const ek = aes256_expand_key(key);

	std::array<unsigned char, 16> h_in{};
	std::array<unsigned char, 16> h{};
	aes256_encrypt_block(ek, h_in.data(), h.data());

	std::array<unsigned char, 16> j0{};
	std::ranges::copy(iv, j0.begin());
	j0[15] = 1;

	std::array<unsigned char, 16> ctr{};
	std::ranges::copy(j0, ctr.begin());

	std::vector<unsigned char> ct(plaintext.size());
	std::array<unsigned char, 16> keystream{};
	for (std::size_t i = 0; i < plaintext.size(); i += 16) {
		gcm_inc32(ctr.data());
		aes256_encrypt_block(ek, ctr.data(), keystream.data());
		std::size_t const chunk = std::min(std::size_t{16}, plaintext.size() - i);
		for (std::size_t j = 0; j < chunk; ++j) {
			ct[i + j] = static_cast<unsigned char>(plaintext[i + j] ^ keystream[j]);
		}
	}

	std::array<unsigned char, 16> ghash_state{};
	if (!aad.empty()) {
		ghash_update(ghash_state.data(), h.data(), aad);
	}
	ghash_update(ghash_state.data(), h.data(), ct);

	std::array<unsigned char, 16> len_block{};
	std::uint64_t const aad_bits = aad.size() * 8;
	std::uint64_t const ct_bits = ct.size() * 8;
	for (int i = 0; i < 8; ++i) {
		len_block[static_cast<std::size_t>(i)] = static_cast<unsigned char>(aad_bits >> (56 - i * 8));
		len_block[static_cast<std::size_t>(i) + 8] = static_cast<unsigned char>(ct_bits >> (56 - i * 8));
	}
	for (std::size_t i = 0; i < 16; ++i) {
		ghash_state[i] = static_cast<unsigned char>(ghash_state[i] ^ len_block[i]);
	}
	ghash_mult(ghash_state.data(), h.data(), ghash_state.data());

	std::array<unsigned char, 16> tag{};
	aes256_encrypt_block(ek, j0.data(), tag.data());
	for (std::size_t i = 0; i < 16; ++i) {
		tag[i] = static_cast<unsigned char>(tag[i] ^ ghash_state[i]);
	}

	std::vector<unsigned char> out;
	out.reserve(ct.size() + 16);
	out.insert(out.end(), ct.begin(), ct.end());
	out.insert(out.end(), tag.begin(), tag.end());
	return out;
#else
	return std::unexpected(std::string{"aes_gcm_encrypt: AES-NI path selected without CPU dispatch fallback"});
#endif
}
std::expected<std::vector<unsigned char>, std::string> aes_gcm_decrypt(
	std::span<unsigned char const> key,
	std::span<unsigned char const> iv,
	std::span<unsigned char const> ciphertext_and_tag,
	std::span<unsigned char const> aad) {
	if (key.size() != 32) {
		return std::unexpected(std::string{"aes_gcm_decrypt: key must be 32 bytes"});
	}
	if (iv.size() != 12) {
		return std::unexpected(std::string{"aes_gcm_decrypt: iv must be 12 bytes"});
	}
	if (ciphertext_and_tag.size() < 16) {
		return std::unexpected(std::string{"aes_gcm_decrypt: input too short (need at least tag)"});
	}

#if defined(CONFLUX_CRYPTO_USE_AESNI)
	#if CONFLUX_CPU_FEATURE_PROBES_RUNTIME
	if (conflux_cpu_supports_aesni_pclmul_sse41())
	#endif
	{
		std::size_t const ct_len = ciphertext_and_tag.size() - 16;
		std::vector<unsigned char> pt(ct_len);
		int const rc = conflux_aes_gcm_decrypt_aesni(
			key.data(),
			iv.data(),
			ciphertext_and_tag.data(),
			ciphertext_and_tag.size(),
			aad.data(),
			aad.size(),
			pt.data());
		if (rc != 0) {
			return std::unexpected(std::string{"aes_gcm_decrypt: authentication failed"});
		}
		return pt;
	}
#endif

#if !defined(CONFLUX_CRYPTO_USE_AESNI) || CONFLUX_CPU_FEATURE_PROBES_RUNTIME
	std::size_t const ct_len = ciphertext_and_tag.size() - 16;
	auto const ct = ciphertext_and_tag.subspan(0, ct_len);
	auto const claimed_tag = ciphertext_and_tag.subspan(ct_len, 16);

	auto const ek = aes256_expand_key(key);

	std::array<unsigned char, 16> h_in{};
	std::array<unsigned char, 16> h{};
	aes256_encrypt_block(ek, h_in.data(), h.data());

	std::array<unsigned char, 16> j0{};
	std::ranges::copy(iv, j0.begin());
	j0[15] = 1;

	// Verify tag before decrypting (authenticate-then-decrypt)
	std::array<unsigned char, 16> ghash_state{};
	if (!aad.empty()) {
		ghash_update(ghash_state.data(), h.data(), aad);
	}
	ghash_update(ghash_state.data(), h.data(), ct);

	std::array<unsigned char, 16> len_block{};
	std::uint64_t const aad_bits = aad.size() * 8;
	std::uint64_t const ct_bits = ct.size() * 8;
	for (int i = 0; i < 8; ++i) {
		len_block[static_cast<std::size_t>(i)] = static_cast<unsigned char>(aad_bits >> (56 - i * 8));
		len_block[static_cast<std::size_t>(i) + 8] = static_cast<unsigned char>(ct_bits >> (56 - i * 8));
	}
	for (std::size_t i = 0; i < 16; ++i) {
		ghash_state[i] = static_cast<unsigned char>(ghash_state[i] ^ len_block[i]);
	}
	ghash_mult(ghash_state.data(), h.data(), ghash_state.data());

	std::array<unsigned char, 16> expected_tag{};
	aes256_encrypt_block(ek, j0.data(), expected_tag.data());
	for (std::size_t i = 0; i < 16; ++i) {
		expected_tag[i] = static_cast<unsigned char>(expected_tag[i] ^ ghash_state[i]);
	}

	// Constant-time tag comparison
	unsigned char tag_diff = 0;
	for (std::size_t i = 0; i < 16; ++i) {
		tag_diff = static_cast<unsigned char>(tag_diff | (expected_tag[i] ^ claimed_tag[i]));
	}
	if (tag_diff != 0) {
		return std::unexpected(std::string{"aes_gcm_decrypt: authentication failed"});
	}

	// Decrypt CTR
	std::array<unsigned char, 16> ctr{};
	std::ranges::copy(j0, ctr.begin());

	std::vector<unsigned char> pt(ct_len);
	std::array<unsigned char, 16> keystream{};
	for (std::size_t i = 0; i < ct_len; i += 16) {
		gcm_inc32(ctr.data());
		aes256_encrypt_block(ek, ctr.data(), keystream.data());
		std::size_t const chunk = std::min(std::size_t{16}, ct_len - i);
		for (std::size_t j = 0; j < chunk; ++j) {
			pt[i + j] = static_cast<unsigned char>(ct[i + j] ^ keystream[j]);
		}
	}
	return pt;
#else
	return std::unexpected(std::string{"aes_gcm_decrypt: AES-NI path selected without CPU dispatch fallback"});
#endif
}

} // namespace conflux::crypto
