// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.crypto;
// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------

TEST_CASE(
	"crypto: base64_encode empty",
	"[crypto]") {
	CHECK(base64_encode({}).empty());
}
TEST_CASE(
	"crypto: base64_encode known V",
	"[crypto]") {
	std::string_view const s = "Man";
	CHECK(base64_encode({reinterpret_cast<unsigned char const *>(s.data()), s.size()}) == "TWFu");
}
TEST_CASE(
	"crypto: base64_encode with padding",
	"[crypto]") {
	std::string_view const s = "Ma";
	CHECK(base64_encode({reinterpret_cast<unsigned char const *>(s.data()), s.size()}) == "TWE=");
}
TEST_CASE(
	"crypto: base64_encode single byte",
	"[crypto]") {
	std::string_view const s = "M";
	CHECK(base64_encode({reinterpret_cast<unsigned char const *>(s.data()), s.size()}) == "TQ==");
}
TEST_CASE(
	"crypto: base64_decode round-trip",
	"[crypto]") {
	std::string const orig = "Hello, World!";
	auto enc = base64_encode({reinterpret_cast<unsigned char const *>(orig.data()), orig.size()});
	CHECK(base64_decode(enc) == orig);
}
TEST_CASE(
	"crypto: base64_decode known V",
	"[crypto]") {
	CHECK(base64_decode("TWFu") == "Man");
	CHECK(base64_decode("TWE=") == "Ma");
	CHECK(base64_decode("TQ==") == "M");
}
TEST_CASE(
	"crypto: base64_decode empty",
	"[crypto]") {
	CHECK(base64_decode("").empty());
}
// ---------------------------------------------------------------------------
// base64url
// ---------------------------------------------------------------------------

TEST_CASE(
	"crypto: base64url_encode no padding, url-safe chars",
	"[crypto]") {
	std::array<unsigned char, 2> raw{0xFB, 0xFF};
	auto enc = base64url_encode(raw);
	CHECK(enc.find('+') == std::string::npos);
	CHECK(enc.find('/') == std::string::npos);
	CHECK(enc.find('=') == std::string::npos);
}
TEST_CASE(
	"crypto: base64url round-trip",
	"[crypto]") {
	std::string const orig = "abc\xfb\xff\xfe";
	auto enc = base64url_encode({reinterpret_cast<unsigned char const *>(orig.data()), orig.size()});
	CHECK(base64url_decode(enc) == orig);
}
TEST_CASE(
	"crypto: base64url_decode known RFC V",
	"[crypto]") {
	// RFC 4648 §10: "" => ""
	CHECK(base64url_decode("").empty());
	// "f" => "Zg"
	CHECK(base64url_decode("Zg") == "f");
}
// ---------------------------------------------------------------------------
// SHA-1
// ---------------------------------------------------------------------------

TEST_CASE(
	"crypto: sha1 empty",
	"[crypto]") {
	// FIPS 180-4 example: SHA1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709
	auto h = sha1({});
	CHECK(h[0] == 0xda);
	CHECK(h[1] == 0x39);
	CHECK(h[19] == 0x09);
}
TEST_CASE(
	"crypto: sha1 abc",
	"[crypto]") {
	// SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
	std::string_view const s = "abc";
	auto h = sha1({reinterpret_cast<unsigned char const *>(s.data()), s.size()});
	CHECK(h[0] == 0xa9);
	CHECK(h[1] == 0x99);
	CHECK(h[2] == 0x3e);
	CHECK(h[19] == 0x9d);
}
TEST_CASE(
	"crypto: sha1 448-bit boundary message",
	"[crypto]") {
	// SHA1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") =
	// 84983e441c3bd26ebaae4aa1f95129e5e54670f1
	std::string_view const s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	auto h = sha1({reinterpret_cast<unsigned char const *>(s.data()), s.size()});
	CHECK(h[0] == 0x84);
	CHECK(h[19] == 0xf1);
}
// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

TEST_CASE(
	"crypto: sha256 empty",
	"[crypto]") {
	// SHA256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
	auto h = sha256({});
	CHECK(h[0] == 0xe3);
	CHECK(h[1] == 0xb0);
	CHECK(h[31] == 0x55);
}
TEST_CASE(
	"crypto: sha256 abc",
	"[crypto]") {
	// SHA256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
	std::string_view const s = "abc";
	auto h = sha256({reinterpret_cast<unsigned char const *>(s.data()), s.size()});
	CHECK(h[0] == 0xba);
	CHECK(h[1] == 0x78);
	CHECK(h[2] == 0x16);
	CHECK(h[3] == 0xbf);
	CHECK(h[31] == 0xad);
}
TEST_CASE(
	"crypto: sha256 448-bit boundary",
	"[crypto]") {
	// SHA256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") =
	// 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1
	std::string_view const s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	auto h = sha256({reinterpret_cast<unsigned char const *>(s.data()), s.size()});
	CHECK(h[0] == 0x24);
	CHECK(h[31] == 0xc1);
}
// ---------------------------------------------------------------------------
// HMAC-SHA256
// ---------------------------------------------------------------------------

TEST_CASE(
	"crypto: hmac_sha256 rfc4231 test V 1",
	"[crypto]") {
	// Key = 20 bytes of 0x0b, Data = "Hi There"
	// HMAC = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
	std::array<unsigned char, 20> key{};
	key.fill(0x0b);
	std::string_view const data = "Hi There";
	auto h = hmac_sha256({key.data(), key.size()}, {reinterpret_cast<unsigned char const *>(data.data()), data.size()});
	CHECK(h[0] == 0xb0);
	CHECK(h[1] == 0x34);
	CHECK(h[31] == 0xf7);
}
TEST_CASE(
	"crypto: hmac_sha256 rfc4231 test V 2",
	"[crypto]") {
	// Key = "Jefe", Data = "what do ya want for nothing?"
	// HMAC = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
	std::string_view const key = "Jefe";
	std::string_view const data = "what do ya want for nothing?";
	auto h = hmac_sha256(
		{reinterpret_cast<unsigned char const *>(key.data()), key.size()},
		{reinterpret_cast<unsigned char const *>(data.data()), data.size()});
	CHECK(h[0] == 0x5b);
	CHECK(h[31] == 0x43);
}
TEST_CASE(
	"crypto: hmac_sha256 same key different data differs",
	"[crypto]") {
	std::string_view const key = "secret";
	std::string_view const d1 = "message1";
	std::string_view const d2 = "message2";
	auto h1 = hmac_sha256(
		{reinterpret_cast<unsigned char const *>(key.data()), key.size()},
		{reinterpret_cast<unsigned char const *>(d1.data()), d1.size()});
	auto h2 = hmac_sha256(
		{reinterpret_cast<unsigned char const *>(key.data()), key.size()},
		{reinterpret_cast<unsigned char const *>(d2.data()), d2.size()});
	CHECK(h1 != h2);
}
// ---------------------------------------------------------------------------
// HMAC-SHA1
// ---------------------------------------------------------------------------

TEST_CASE(
	"crypto: hmac_sha1 rfc2202 test V 1",
	"[crypto]") {
	// Key = 20 bytes of 0x0b, Data = "Hi There"
	// HMAC-SHA1 = b617318655057264e28bc0b6fb378c8ef146be00
	std::array<unsigned char, 20> key{};
	key.fill(0x0b);
	std::string_view const data = "Hi There";
	auto h = hmac_sha1({key.data(), key.size()}, {reinterpret_cast<unsigned char const *>(data.data()), data.size()});
	CHECK(h[0] == 0xb6);
	CHECK(h[1] == 0x17);
	CHECK(h[19] == 0x00);
}
TEST_CASE(
	"crypto: hmac_sha1 rfc2202 test V 2",
	"[crypto]") {
	// Key = "Jefe", Data = "what do ya want for nothing?"
	// HMAC-SHA1 = effcdf6ae5eb2fa2d27416d5f184df9c259a7c79
	std::string_view const key = "Jefe";
	std::string_view const data = "what do ya want for nothing?";
	auto h = hmac_sha1(
		{reinterpret_cast<unsigned char const *>(key.data()), key.size()},
		{reinterpret_cast<unsigned char const *>(data.data()), data.size()});
	CHECK(h[0] == 0xef);
	CHECK(h[1] == 0xfc);
	CHECK(h[19] == 0x79);
}
TEST_CASE(
	"crypto: hmac_sha1 rfc2202 test V 3",
	"[crypto]") {
	// Key = 20 bytes of 0xaa, Data = 50 bytes of 0xdd
	// HMAC-SHA1 = 125d7342b9ac11cd91a39af48aa17b4f63f175d3
	std::array<unsigned char, 20> key{};
	key.fill(0xaa);
	std::array<unsigned char, 50> data{};
	data.fill(0xdd);
	auto h = hmac_sha1({key.data(), key.size()}, {data.data(), data.size()});
	CHECK(h[0] == 0x12);
	CHECK(h[1] == 0x5d);
	CHECK(h[19] == 0xd3);
}
// ---------------------------------------------------------------------------
// AES-256-GCM
// ---------------------------------------------------------------------------

TEST_CASE(
	"crypto: aes_gcm_encrypt/decrypt round-trip",
	"[crypto]") {
	std::array<unsigned char, 32> key{};
	for (std::size_t i = 0; i < 32; ++i) {
		key[i] = static_cast<unsigned char>(i);
	}
	std::array<unsigned char, 12> iv{};
	for (std::size_t i = 0; i < 12; ++i) {
		iv[i] = static_cast<unsigned char>(i + 0x10);
	}
	std::string_view const msg = "Hello AES-GCM!";
	auto ct = aes_gcm_encrypt(key, iv, {reinterpret_cast<unsigned char const *>(msg.data()), msg.size()}, {});
	REQUIRE(ct.has_value());
	REQUIRE(ct->size() == msg.size() + 16);

	auto pt = aes_gcm_decrypt(key, iv, *ct, {});
	REQUIRE(pt.has_value());
	CHECK(std::string(pt->begin(), pt->end()) == msg);
}
TEST_CASE(
	"crypto: aes_gcm_decrypt detects tampered ciphertext",
	"[crypto]") {
	std::array<unsigned char, 32> key{};
	key.fill(0xAA);
	std::array<unsigned char, 12> iv{};
	iv.fill(0xBB);
	std::string_view const msg = "secret data";
	auto ct = aes_gcm_encrypt(key, iv, {reinterpret_cast<unsigned char const *>(msg.data()), msg.size()}, {});
	REQUIRE(ct.has_value());

	// Flip a byte in ciphertext
	(*ct)[0] = static_cast<unsigned char>((*ct)[0] ^ 0xFF);
	auto pt = aes_gcm_decrypt(key, iv, *ct, {});
	CHECK(!pt.has_value());
}
TEST_CASE(
	"crypto: aes_gcm_decrypt detects tampered tag",
	"[crypto]") {
	std::array<unsigned char, 32> key{};
	key.fill(0x11);
	std::array<unsigned char, 12> iv{};
	iv.fill(0x22);
	std::string_view const msg = "authenticate me";
	auto ct = aes_gcm_encrypt(key, iv, {reinterpret_cast<unsigned char const *>(msg.data()), msg.size()}, {});
	REQUIRE(ct.has_value());

	// Flip last byte (in tag)
	ct->back() = static_cast<unsigned char>(ct->back() ^ 1);
	auto pt = aes_gcm_decrypt(key, iv, *ct, {});
	CHECK(!pt.has_value());
}
TEST_CASE(
	"crypto: aes_gcm with AAD",
	"[crypto]") {
	std::array<unsigned char, 32> key{};
	key.fill(0x55);
	std::array<unsigned char, 12> iv{};
	iv.fill(0x66);
	std::string_view const msg = "payload";
	std::string_view const aad = "associated data";
	auto ct = aes_gcm_encrypt(
		key,
		iv,
		{reinterpret_cast<unsigned char const *>(msg.data()), msg.size()},
		{reinterpret_cast<unsigned char const *>(aad.data()), aad.size()});
	REQUIRE(ct.has_value());

	// Decrypt with correct AAD
	auto pt = aes_gcm_decrypt(key, iv, *ct, {reinterpret_cast<unsigned char const *>(aad.data()), aad.size()});
	REQUIRE(pt.has_value());
	CHECK(std::string(pt->begin(), pt->end()) == msg);

	// Decrypt with wrong AAD fails
	std::string_view const bad_aad = "wrong data";
	auto pt2 = aes_gcm_decrypt(key, iv, *ct, {reinterpret_cast<unsigned char const *>(bad_aad.data()), bad_aad.size()});
	CHECK(!pt2.has_value());
}
TEST_CASE(
	"crypto: aes_gcm NIST test vector (AES-256, 96-bit IV)",
	"[crypto]") {
	// NIST SP 800-38D, Test Case 16
	// Key = 0000...00 (32 bytes), IV = 0000...00 (12 bytes), PT = empty, AAD = empty
	// Expected CT = empty, Tag = 530f8afbc74536b9a963b4f1c4cb738b
	std::array<unsigned char, 32> key{};
	std::array<unsigned char, 12> iv{};
	auto ct = aes_gcm_encrypt(key, iv, {}, {});
	REQUIRE(ct.has_value());
	REQUIRE(ct->size() == 16); // tag only
	CHECK((*ct)[0] == 0x53);
	CHECK((*ct)[1] == 0x0f);
	CHECK((*ct)[2] == 0x8a);
	CHECK((*ct)[3] == 0xfb);
	CHECK((*ct)[14] == 0x73);
	CHECK((*ct)[15] == 0x8b);
}
TEST_CASE(
	"crypto: aes_gcm empty plaintext decrypt",
	"[crypto]") {
	std::array<unsigned char, 32> key{};
	std::array<unsigned char, 12> iv{};
	auto ct = aes_gcm_encrypt(key, iv, {}, {});
	REQUIRE(ct.has_value());
	auto pt = aes_gcm_decrypt(key, iv, *ct, {});
	REQUIRE(pt.has_value());
	CHECK(pt->empty());
}
TEST_CASE(
	"crypto: aes_gcm rejects wrong key size",
	"[crypto]") {
	std::array<unsigned char, 16> short_key{};
	std::array<unsigned char, 12> iv{};
	auto r = aes_gcm_encrypt(short_key, iv, {}, {});
	CHECK(!r.has_value());
}
TEST_CASE(
	"crypto: aes_gcm rejects wrong IV size",
	"[crypto]") {
	std::array<unsigned char, 32> key{};
	std::array<unsigned char, 8> short_iv{};
	auto r = aes_gcm_encrypt(key, short_iv, {}, {});
	CHECK(!r.has_value());
}
