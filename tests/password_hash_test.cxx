// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.net.password_hash;

TEST_CASE(
	"password_hash: PBKDF2-SHA256 vector is stable",
	"[auth][password_hash]") {
	auto opts = pbkdf2_sha256_password_hash_options(1);
	opts.salt_bytes = 4;
	opts.hash_bytes = 32;

	auto encoded = password_hash_with_salt("password", "salt", opts);
	REQUIRE(encoded.has_value());
	CHECK(*encoded == "$pbkdf2-sha256$v=1$i=1,l=32$c2FsdA$Eg-2z_z4syxD5yJSVsT4N6hlSMkszDVICAWYfLcL4Xs");
}

TEST_CASE(
	"password_hash: verify accepts matching password and rejects wrong password",
	"[auth][password_hash]") {
	auto opts = pbkdf2_sha256_password_hash_options(2);
	opts.salt_bytes = 8;
	opts.hash_bytes = 32;

	auto encoded = password_hash_with_salt("correct horse", "12345678", opts);
	REQUIRE(encoded.has_value());

	auto ok = password_verify("correct horse", *encoded, opts);
	REQUIRE(ok.has_value());
	CHECK(ok->ok);
	CHECK_FALSE(ok->needs_rehash);

	auto bad = password_verify("wrong horse", *encoded, opts);
	REQUIRE(bad.has_value());
	CHECK_FALSE(bad->ok);
	CHECK_FALSE(bad->needs_rehash);
}

TEST_CASE(
	"password_hash: verify reports upgrade path when parameters change",
	"[auth][password_hash]") {
	auto old_opts = pbkdf2_sha256_password_hash_options(1);
	old_opts.salt_bytes = 8;
	old_opts.hash_bytes = 32;
	auto current_opts = pbkdf2_sha256_password_hash_options(2);
	current_opts.salt_bytes = 8;
	current_opts.hash_bytes = 32;

	auto encoded = password_hash_with_salt("secret", "abcdefgh", old_opts);
	REQUIRE(encoded.has_value());

	auto verified = password_verify("secret", *encoded, current_opts);
	REQUIRE(verified.has_value());
	CHECK(verified->ok);
	CHECK(verified->needs_rehash);

	auto needs = password_needs_rehash(*encoded, current_opts);
	REQUIRE(needs.has_value());
	CHECK(*needs);
}

TEST_CASE(
	"password_hash: malformed encoded hashes are rejected",
	"[auth][password_hash]") {
	auto opts = pbkdf2_sha256_password_hash_options(1);
	auto malformed = password_verify("pw", "$pbkdf2-sha256$v=1$i=1,l=32$not valid$hash", opts);
	CHECK_FALSE(malformed.has_value());

	auto unsupported = password_verify("pw", "$md5$v=1$i=1,l=16$c2FsdA$aaaa", opts);
	CHECK_FALSE(unsupported.has_value());
}

TEST_CASE(
	"password_hash: Argon2id runtime path verifies when libargon2 is present",
	"[auth][password_hash]") {
	if (!password_hash_argon2id_available()) {
		SUCCEED("libargon2 runtime library is not available");
		return;
	}

	PasswordHashOptions opts;
	opts.memory_kib = 64;
	opts.iterations = 1;
	opts.parallelism = 1;
	opts.salt_bytes = 8;
	opts.hash_bytes = 16;

	auto encoded = password_hash_with_salt("argon secret", "12345678", opts);
	REQUIRE(encoded.has_value());
	CHECK(encoded->starts_with("$argon2id$v=19$m=64,t=1,p=1$"));

	auto ok = password_verify("argon secret", *encoded, opts);
	REQUIRE(ok.has_value());
	CHECK(ok->ok);
	CHECK_FALSE(ok->needs_rehash);

	auto bad = password_verify("wrong", *encoded, opts);
	REQUIRE(bad.has_value());
	CHECK_FALSE(bad->ok);
}
