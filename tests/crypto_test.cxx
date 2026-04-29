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
	SV const s = "Man";
	CHECK(base64_encode({reinterpret_cast<unsigned char const *>(s.data()), s.size()}) == "TWFu");
}

TEST_CASE(
	"crypto: base64_encode with padding",
	"[crypto]") {
	SV const s = "Ma";
	CHECK(base64_encode({reinterpret_cast<unsigned char const *>(s.data()), s.size()}) == "TWE=");
}

TEST_CASE(
	"crypto: base64_encode single byte",
	"[crypto]") {
	SV const s = "M";
	CHECK(base64_encode({reinterpret_cast<unsigned char const *>(s.data()), s.size()}) == "TQ==");
}

TEST_CASE(
	"crypto: base64_decode round-trip",
	"[crypto]") {
	S const orig = "Hello, World!";
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
	A<unsigned char, 2> raw{0xFB, 0xFF};
	auto enc = base64url_encode(raw);
	CHECK(enc.find('+') == S::npos);
	CHECK(enc.find('/') == S::npos);
	CHECK(enc.find('=') == S::npos);
}

TEST_CASE(
	"crypto: base64url round-trip",
	"[crypto]") {
	S const orig = "abc\xfb\xff\xfe";
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
	SV const s = "abc";
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
	SV const s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
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
	SV const s = "abc";
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
	SV const s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
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
	A<unsigned char, 20> key{};
	key.fill(0x0b);
	SV const data = "Hi There";
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
	SV const key = "Jefe";
	SV const data = "what do ya want for nothing?";
	auto h = hmac_sha256(
		{reinterpret_cast<unsigned char const *>(key.data()), key.size()},
		{reinterpret_cast<unsigned char const *>(data.data()), data.size()});
	CHECK(h[0] == 0x5b);
	CHECK(h[31] == 0x43);
}

TEST_CASE(
	"crypto: hmac_sha256 same key different data differs",
	"[crypto]") {
	SV const key = "secret";
	SV const d1 = "message1";
	SV const d2 = "message2";
	auto h1 = hmac_sha256(
		{reinterpret_cast<unsigned char const *>(key.data()), key.size()},
		{reinterpret_cast<unsigned char const *>(d1.data()), d1.size()});
	auto h2 = hmac_sha256(
		{reinterpret_cast<unsigned char const *>(key.data()), key.size()},
		{reinterpret_cast<unsigned char const *>(d2.data()), d2.size()});
	CHECK(h1 != h2);
}
