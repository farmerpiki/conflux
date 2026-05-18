// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.crypto;
import conflux.net.jwt;
import conflux.net.config;
// ---------------------------------------------------------------------------
// jwt_sign (2-arg: default header)
// ---------------------------------------------------------------------------

TEST_CASE(
	"jwt: sign and decode round-trip",
	"[jwt]") {
	std::string const secret = "test-secret-key-32bytes";
	std::string const payload = R"({"sub":"user1","iss":"test"})";
	auto token = jwt_sign(payload, secret);

	JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;
	auto result = jwt_decode(token, opts);
	REQUIRE(result.has_value());
	CHECK(result->sub == "user1");
	CHECK(result->iss == "test");
}
// ---------------------------------------------------------------------------
// jwt_sign (3-arg: custom header)
// ---------------------------------------------------------------------------

TEST_CASE(
	"jwt: sign with custom header round-trip",
	"[jwt]") {
	std::string const secret = "my-secret-key-32bytes";
	std::string const header = R"({"alg":"HS256","typ":"JWT","kid":"key-42"})";
	std::string const payload = R"({"sub":"admin","iss":"ghost"})";
	auto token = jwt_sign(header, payload, secret);

	JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;
	auto result = jwt_decode(token, opts);
	REQUIRE(result.has_value());
	CHECK(result->sub == "admin");
	CHECK(result->iss == "ghost");
}
TEST_CASE(
	"jwt: custom header preserves kid in base64url-decoded header",
	"[jwt]") {
	std::string const secret = "secret-key-32bytes";
	std::string const header = R"({"alg":"HS256","typ":"JWT","kid":"key-99"})";
	std::string const payload = R"({"sub":"x"})";
	auto token = jwt_sign(header, payload, secret);

	auto dot1 = token.find('.');
	REQUIRE(dot1 != std::string::npos);
	auto header_b64 = std::string_view{token}.substr(0, dot1);
	auto decoded_header = base64url_decode(header_b64);
	CHECK(decoded_header.find("key-99") != std::string::npos);
}
TEST_CASE(
	"jwt: wrong secret fails verification",
	"[jwt]") {
	std::string const payload = R"({"sub":"u"})";
	auto token = jwt_sign(payload, "correct-secret-32bytes");

	JwtOptions opts;
	opts.secrets = single_secret_rotation("wrong-secret-32bytes");
	opts.verify_exp = false;
	auto result = jwt_decode(token, opts);
	CHECK(!result.has_value());
}

namespace {

std::int64_t jwt_test_now() {
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

TEST_CASE(
	"jwt: strict session policy requires lifetime claims and supports revocation",
	"[jwt][auth]") {
	std::string const secret = "session-jwt-secret-32bytes";
	auto const now = jwt_test_now();

	JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.require_exp = true;
	opts.require_iat = true;
	opts.require_jti = true;
	opts.max_token_lifetime = std::chrono::minutes{5};

	auto missing_claims = jwt_decode(jwt_sign(R"({"sub":"u"})", secret), opts);
	REQUIRE_FALSE(missing_claims.has_value());
	CHECK(missing_claims.error() == "missing exp claim");

	auto valid = jwt_decode(
		jwt_sign(format(R"({{"sub":"u","jti":"active","iat":{},"exp":{}}})", now, now + 120), secret),
		opts);
	REQUIRE(valid.has_value());
	CHECK(valid->jti == "active");

	auto too_long = jwt_decode(
		jwt_sign(format(R"({{"sub":"u","jti":"long","iat":{},"exp":{}}})", now, now + 600), secret),
		opts);
	REQUIRE_FALSE(too_long.has_value());
	CHECK(too_long.error() == "token lifetime too long");

	opts.revoked_jti = [](std::string_view jti) { return jti == "revoked"; };
	auto revoked = jwt_decode(
		jwt_sign(format(R"({{"sub":"u","jti":"revoked","iat":{},"exp":{}}})", now, now + 120), secret),
		opts);
	REQUIRE_FALSE(revoked.has_value());
	CHECK(revoked.error() == "token revoked");
}

TEST_CASE(
	"jwt: clock skew applies to exp and nbf boundaries",
	"[jwt][auth]") {
	std::string const secret = "session-jwt-secret-32bytes";
	auto const now = jwt_test_now();
	auto token = jwt_sign(format(R"({{"sub":"u","iat":{},"exp":{},"nbf":{}}})", now - 60, now - 5, now + 5), secret);

	JwtOptions strict;
	strict.secrets = single_secret_rotation(secret);
	strict.require_exp = true;
	strict.require_iat = true;
	auto rejected = jwt_decode(token, strict);
	REQUIRE_FALSE(rejected.has_value());
	CHECK(rejected.error() == "token expired");

	JwtOptions skewed = strict;
	skewed.clock_skew = std::chrono::seconds{60};
	auto accepted = jwt_decode(token, skewed);
	REQUIRE(accepted.has_value());
	CHECK(accepted->sub == "u");
}

TEST_CASE(
	"jwt: negative registered time claims are rejected",
	"[jwt][auth]") {
	std::string const secret = "session-jwt-secret-32bytes";
	JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;

	auto exp = jwt_decode(jwt_sign(R"({"sub":"u","exp":-1})", secret), opts);
	REQUIRE_FALSE(exp.has_value());
	CHECK(exp.error() == "invalid exp claim");

	auto nbf = jwt_decode(jwt_sign(R"({"sub":"u","nbf":-1})", secret), opts);
	REQUIRE_FALSE(nbf.has_value());
	CHECK(nbf.error() == "invalid nbf claim");

	auto iat = jwt_decode(jwt_sign(R"({"sub":"u","iat":-1})", secret), opts);
	REQUIRE_FALSE(iat.has_value());
	CHECK(iat.error() == "invalid iat claim");
}
