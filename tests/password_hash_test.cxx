// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.net.password_hash;

TEST_CASE(
	"conflux::http::password_hash: PBKDF2-SHA256 vector is stable",
	"[auth][conflux::http::password_hash]") {
	auto opts = conflux::http::pbkdf2_sha256_password_hash_options(1);
	opts.salt_bytes = 4;
	opts.hash_bytes = 32;

	auto encoded = conflux::http::password_hash_with_salt("password", "salt", opts);
	REQUIRE(encoded.has_value());
	CHECK(*encoded == "$pbkdf2-sha256$v=1$i=1,l=32$c2FsdA$Eg-2z_z4syxD5yJSVsT4N6hlSMkszDVICAWYfLcL4Xs");
}

TEST_CASE(
	"conflux::http::password_hash: verify accepts matching password and rejects wrong password",
	"[auth][conflux::http::password_hash]") {
	auto opts = conflux::http::pbkdf2_sha256_password_hash_options(2);
	opts.salt_bytes = 8;
	opts.hash_bytes = 32;

	auto encoded = conflux::http::password_hash_with_salt("correct horse", "12345678", opts);
	REQUIRE(encoded.has_value());

	auto ok = conflux::http::password_verify("correct horse", *encoded, opts);
	REQUIRE(ok.has_value());
	CHECK(ok->ok);
	CHECK_FALSE(ok->needs_rehash);

	auto bad = conflux::http::password_verify("wrong horse", *encoded, opts);
	REQUIRE(bad.has_value());
	CHECK_FALSE(bad->ok);
	CHECK_FALSE(bad->needs_rehash);
}

TEST_CASE(
	"conflux::http::password_hash: verify reports upgrade path when parameters change",
	"[auth][conflux::http::password_hash]") {
	auto old_opts = conflux::http::pbkdf2_sha256_password_hash_options(1);
	old_opts.salt_bytes = 8;
	old_opts.hash_bytes = 32;
	auto current_opts = conflux::http::pbkdf2_sha256_password_hash_options(2);
	current_opts.salt_bytes = 8;
	current_opts.hash_bytes = 32;

	auto encoded = conflux::http::password_hash_with_salt("secret", "abcdefgh", old_opts);
	REQUIRE(encoded.has_value());

	auto verified = conflux::http::password_verify("secret", *encoded, current_opts);
	REQUIRE(verified.has_value());
	CHECK(verified->ok);
	CHECK(verified->needs_rehash);

	auto needs = conflux::http::password_needs_rehash(*encoded, current_opts);
	REQUIRE(needs.has_value());
	CHECK(*needs);
}

TEST_CASE(
	"conflux::http::password_hash: verifier secret marks new hashes and rejects unmarked hashes",
	"[auth][conflux::http::password_hash]") {
	auto opts = conflux::http::pbkdf2_sha256_password_hash_options(1);
	opts.salt_bytes = 8;
	opts.hash_bytes = 32;
	conflux::http::PasswordHashSecrets secrets{.verifier_secret = "server-side pepper"};

	auto encoded = conflux::http::password_hash_with_salt("secret", "abcdefgh", opts, secrets);
	REQUIRE(encoded.has_value());
	CHECK(encoded->starts_with("$pbkdf2-sha256$v=1$i=1,l=32,k=1$"));

	auto ok = conflux::http::password_verify("secret", *encoded, opts, secrets);
	REQUIRE(ok.has_value());
	CHECK(ok->ok);
	CHECK_FALSE(ok->needs_rehash);

	auto missing_secret = conflux::http::pbkdf2_sha256_password_hash_options(1);
	missing_secret.salt_bytes = 8;
	missing_secret.hash_bytes = 32;
	auto rejected = conflux::http::password_verify("secret", *encoded, missing_secret);
	CHECK_FALSE(rejected.has_value());

	auto legacy = conflux::http::password_hash_with_salt("secret", "abcdefgh", missing_secret);
	REQUIRE(legacy.has_value());
	auto migrated = conflux::http::password_verify("secret", *legacy, opts, secrets);
	REQUIRE(migrated.has_value());
	CHECK_FALSE(migrated->ok);
	CHECK_FALSE(migrated->needs_rehash);
}

TEST_CASE(
	"conflux::http::password_hash: resource limits are configurable",
	"[auth][conflux::http::password_hash]") {
	auto configured =
		conflux::http::password_hash_configure_resource_limits({.max_concurrent_hashes = 1, .max_waiting_hashes = 0});
	REQUIRE(configured.has_value());
	auto current = conflux::http::password_hash_resource_limits();
	CHECK(current.max_concurrent_hashes == 1);
	CHECK(current.max_waiting_hashes == 0);

	auto reset = conflux::http::password_hash_configure_resource_limits({});
	REQUIRE(reset.has_value());
	CHECK(conflux::http::password_hash_resource_limits().max_concurrent_hashes >= 1);
}

TEST_CASE(
	"conflux::http::password_hash: malformed encoded hashes are rejected",
	"[auth][conflux::http::password_hash]") {
	auto opts = conflux::http::pbkdf2_sha256_password_hash_options(1);
	auto malformed = conflux::http::password_verify("pw", "$pbkdf2-sha256$v=1$i=1,l=32$not valid$hash", opts);
	CHECK_FALSE(malformed.has_value());

	auto unsupported = conflux::http::password_verify("pw", "$md5$v=1$i=1,l=16$c2FsdA$aaaa", opts);
	CHECK_FALSE(unsupported.has_value());
}

TEST_CASE(
	"conflux::http::password_hash: Argon2id path verifies when libargon2 is present",
	"[auth][conflux::http::password_hash]") {
	if (!conflux::http::password_hash_argon2id_available()) {
		SUCCEED("libargon2 backend is not available");
		return;
	}

	conflux::http::PasswordHashOptions opts;
	opts.memory_kib = 64;
	opts.iterations = 1;
	opts.parallelism = 1;
	opts.salt_bytes = 8;
	opts.hash_bytes = 16;

	auto encoded = conflux::http::password_hash_with_salt("argon secret", "12345678", opts);
	REQUIRE(encoded.has_value());
	CHECK(encoded->starts_with("$argon2id$v=19$m=64,t=1,p=1$"));

	auto ok = conflux::http::password_verify("argon secret", *encoded, opts);
	REQUIRE(ok.has_value());
	CHECK(ok->ok);
	CHECK_FALSE(ok->needs_rehash);

	auto bad = conflux::http::password_verify("wrong", *encoded, opts);
	REQUIRE(bad.has_value());
	CHECK_FALSE(bad->ok);
}
