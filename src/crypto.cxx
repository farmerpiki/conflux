module;
#include <cstdint>

export module conflux.crypto;
import std;
import conflux.types;
import std.compat;

namespace {

constexpr SV kB64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr SV kB64UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr A<i8, 256> make_b64_table(
	SV alphabet) {
	A<i8, 256> t{};
	t.fill(-1);
	for (SZ i = 0; i < 64; ++i) {
		t[static_cast<unsigned char>(alphabet[i])] = static_cast<i8>(i);
	}
	return t;
}

constexpr auto kB64Table = make_b64_table(kB64Alphabet);
constexpr auto kB64UrlTable = make_b64_table(kB64UrlAlphabet);

S b64_encode_impl(
	span<unsigned char const> in,
	SV alphabet,
	bool padding) {
	S out;
	out.reserve(((in.size() + 2) / 3) * 4);
	for (SZ i = 0; i < in.size(); i += 3) {
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

S b64_decode_impl(
	SV encoded,
	span<i8 const, 256> table) {
	S out;
	out.reserve(((encoded.size() * 3) / 4) + 1);
	int bits = 0;
	int val = 0;
	for (char const raw: encoded) {
		auto c = static_cast<unsigned char>(raw);
		if (c == '=') {
			break;
		}
		i8 const d = table[c];
		if (d < 0) {
			return {};
		}
		val = (val << 6) | d;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += static_cast<char>((val >> bits) & 0xFF);
		}
	}
	return out;
}

} // namespace

export S base64_encode(
	span<unsigned char const> in) {
	return b64_encode_impl(in, kB64Alphabet, true);
}

export S base64_decode(
	SV encoded) {
	return b64_decode_impl(encoded, kB64Table);
}

export S base64url_encode(
	span<unsigned char const> in) {
	return b64_encode_impl(in, kB64UrlAlphabet, false);
}

export S base64url_decode(
	SV encoded) {
	return b64_decode_impl(encoded, kB64UrlTable);
}

// ---------------------------------------------------------------------------
// SHA-1 (FIPS 180-4)
// ---------------------------------------------------------------------------

export A<unsigned char, 20> sha1(
	span<unsigned char const> msg) {
	A<u32, 5> h{0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};

	V<unsigned char> padded;
	padded.reserve(msg.size() + 72);
	padded.insert(padded.end(), msg.begin(), msg.end());
	padded.push_back(0x80U);
	while ((padded.size() % 64) != 56) {
		padded.push_back(0);
	}
	u64 const bit_len = msg.size() * 8ULL;
	for (int s = 56; s >= 0; s -= 8) {
		padded.push_back(static_cast<unsigned char>((bit_len >> s) & 0xFF));
	}

	auto rot32 = [](u32 v, unsigned n) -> u32 { return (v << n) | (v >> (32 - n)); };

	for (SZ blk = 0; blk < padded.size(); blk += 64) {
		A<u32, 80> w{};
		for (int i = 0; i < 16; ++i) {
			auto b = span{padded}.subspan(blk + (static_cast<SZ>(i) * 4), 4);
			w[static_cast<SZ>(i)] = (static_cast<u32>(b[0]) << 24)
								  | (static_cast<u32>(b[1]) << 16)
								  | (static_cast<u32>(b[2]) << 8)
								  | static_cast<u32>(b[3]);
		}
		for (SZ i = 16; i < 80; ++i) {
			w[i] = rot32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}
		auto [a, b, c, d, e] = h;
		for (SZ i = 0; i < 80; ++i) {
			u32 f{};
			u32 k{};
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
			u32 const tmp = rot32(a, 5) + f + e + k + w[i];
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

	A<unsigned char, 20> out{};
	for (SZ i = 0; i < 5; ++i) {
		out[(i * 4) + 0] = static_cast<unsigned char>(h[i] >> 24);
		out[(i * 4) + 1] = static_cast<unsigned char>(h[i] >> 16);
		out[(i * 4) + 2] = static_cast<unsigned char>(h[i] >> 8);
		out[(i * 4) + 3] = static_cast<unsigned char>(h[i]);
	}
	return out;
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

export A<unsigned char, 32> sha256(
	span<unsigned char const> msg) {
	static constexpr A<u32, 64> K{
		0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
		0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
		0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
		0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
		0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
		0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
		0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
		0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
	};

	A<u32, 8> h{
		0x6a09e667U,
		0xbb67ae85U,
		0x3c6ef372U,
		0xa54ff53aU,
		0x510e527fU,
		0x9b05688cU,
		0x1f83d9abU,
		0x5be0cd19U,
	};

	V<unsigned char> padded;
	padded.reserve(msg.size() + 72);
	padded.insert(padded.end(), msg.begin(), msg.end());
	padded.push_back(0x80U);
	while ((padded.size() % 64) != 56) {
		padded.push_back(0);
	}
	u64 const bit_len = msg.size() * 8ULL;
	for (int s = 56; s >= 0; s -= 8) {
		padded.push_back(static_cast<unsigned char>((bit_len >> s) & 0xFF));
	}

	auto rotr = [](u32 v, unsigned n) -> u32 { return (v >> n) | (v << (32 - n)); };
	auto ch = [](u32 e, u32 f, u32 g) { return (e & f) ^ (~e & g); };
	auto maj = [](u32 a, u32 b, u32 c) { return (a & b) ^ (a & c) ^ (b & c); };

	for (SZ blk = 0; blk < padded.size(); blk += 64) {
		A<u32, 64> w{};
		for (SZ i = 0; i < 16; ++i) {
			auto b = span{padded}.subspan(blk + (i * 4), 4);
			w[i] = (static_cast<u32>(b[0]) << 24)
				 | (static_cast<u32>(b[1]) << 16)
				 | (static_cast<u32>(b[2]) << 8)
				 | static_cast<u32>(b[3]);
		}
		for (SZ i = 16; i < 64; ++i) {
			auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		auto [a, b, c, d, e, f, g, hh] = h;
		for (SZ i = 0; i < 64; ++i) {
			auto S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
			auto S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
			auto temp1 = hh + S1 + ch(e, f, g) + K[i] + w[i];
			auto temp2 = S0 + maj(a, b, c);
			hh = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
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

	A<unsigned char, 32> out{};
	for (SZ i = 0; i < 32; ++i) {
		out[i] = static_cast<unsigned char>((h[i / 4] >> (24 - ((i % 4) * 8))) & 0xFFU);
	}
	return out;
}

// ---------------------------------------------------------------------------
// HMAC-SHA256 (RFC 2104)
// ---------------------------------------------------------------------------

export A<unsigned char, 32> hmac_sha256(
	span<unsigned char const> key,
	span<unsigned char const> msg) {
	A<unsigned char, 64> k_pad{};
	if (key.size() > 64) {
		auto kh = sha256(key);
		ranges::copy(kh, k_pad.begin());
	} else {
		ranges::copy(key, k_pad.begin());
	}

	auto xor_pad = [&](unsigned char mask) {
		V<unsigned char> buf(64 + msg.size());
		for (SZ i = 0; i < 64; ++i) {
			buf[i] = static_cast<unsigned char>(k_pad[i] ^ mask);
		}
		ranges::copy(msg, buf.begin() + 64);
		return buf;
	};

	auto inner = sha256(xor_pad(0x36U));
	V<unsigned char> outer_input(64 + 32);
	for (SZ i = 0; i < 64; ++i) {
		outer_input[i] = static_cast<unsigned char>(k_pad[i] ^ 0x5CU);
	}
	ranges::copy(inner, outer_input.begin() + 64);
	return sha256(outer_input);
}

export bool constant_time_eq(
	SV a,
	SV b) {
	if (a.size() != b.size()) {
		return false;
	}
	unsigned char acc = 0;
	for (SZ i = 0; i < a.size(); ++i) {
		acc = static_cast<unsigned char>(acc | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
	}
	return acc == 0;
}
