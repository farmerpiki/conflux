// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.crypto;
import conflux.net.jwt;
import conflux.net.config;

using conflux::http::single_secret_rotation;
// ---------------------------------------------------------------------------
// jwt_sign (2-arg: default header)
// ---------------------------------------------------------------------------

TEST_CASE(
	"jwt: sign and decode round-trip",
	"[jwt]") {
	std::string const secret = "test-secret-key-32bytes";
	std::string const payload = R"({"sub":"user1","iss":"test"})";
	auto token = conflux::http::jwt_sign(payload, secret);

	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;
	auto result = conflux::http::jwt_decode(token, opts);
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
	auto token = conflux::http::jwt_sign(header, payload, secret);

	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;
	auto result = conflux::http::jwt_decode(token, opts);
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
	auto token = conflux::http::jwt_sign(header, payload, secret);

	auto dot1 = token.find('.');
	REQUIRE(dot1 != std::string::npos);
	auto header_b64 = std::string_view{token}.substr(0, dot1);
	auto decoded_header = conflux::crypto::base64url_decode(header_b64);
	CHECK(decoded_header.find("key-99") != std::string::npos);
}
TEST_CASE(
	"jwt: wrong secret fails verification",
	"[jwt]") {
	std::string const payload = R"({"sub":"u"})";
	auto token = conflux::http::jwt_sign(payload, "correct-secret-32bytes");

	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation("wrong-secret-32bytes");
	opts.verify_exp = false;
	auto result = conflux::http::jwt_decode(token, opts);
	CHECK(!result.has_value());
}

namespace {

std::int64_t jwt_test_now() {
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
		.count();
}

} // namespace

TEST_CASE(
	"jwt: strict session policy requires lifetime claims and supports revocation",
	"[jwt][auth]") {
	std::string const secret = "session-jwt-secret-32bytes";
	auto const now = jwt_test_now();

	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.require_exp = true;
	opts.require_iat = true;
	opts.require_jti = true;
	opts.max_token_lifetime = std::chrono::minutes{5};

	auto missing_claims = conflux::http::jwt_decode(conflux::http::jwt_sign(R"({"sub":"u"})", secret), opts);
	REQUIRE_FALSE(missing_claims.has_value());
	CHECK(missing_claims.error() == "missing exp claim");

	auto valid = conflux::http::jwt_decode(
		conflux::http::jwt_sign(
			std::format(R"({{"sub":"u","jti":"active","iat":{},"exp":{}}})", now, now + 120),
			secret),
		opts);
	REQUIRE(valid.has_value());
	CHECK(valid->jti == "active");

	auto too_long = conflux::http::jwt_decode(
		conflux::http::jwt_sign(std::format(R"({{"sub":"u","jti":"long","iat":{},"exp":{}}})", now, now + 600), secret),
		opts);
	REQUIRE_FALSE(too_long.has_value());
	CHECK(too_long.error() == "token lifetime too long");

	opts.revoked_jti = [](std::string_view jti) { return jti == "revoked"; };
	auto revoked = conflux::http::jwt_decode(
		conflux::http::jwt_sign(
			std::format(R"({{"sub":"u","jti":"revoked","iat":{},"exp":{}}})", now, now + 120),
			secret),
		opts);
	REQUIRE_FALSE(revoked.has_value());
	CHECK(revoked.error() == "token revoked");
}

TEST_CASE(
	"jwt: clock skew applies to exp and nbf boundaries",
	"[jwt][auth]") {
	std::string const secret = "session-jwt-secret-32bytes";
	auto const now = jwt_test_now();
	auto token = conflux::http::jwt_sign(
		std::format(R"({{"sub":"u","iat":{},"exp":{},"nbf":{}}})", now - 60, now - 5, now + 5),
		secret);

	conflux::http::JwtOptions strict;
	strict.secrets = single_secret_rotation(secret);
	strict.require_exp = true;
	strict.require_iat = true;
	auto rejected = conflux::http::jwt_decode(token, strict);
	REQUIRE_FALSE(rejected.has_value());
	CHECK(rejected.error() == "token expired");

	conflux::http::JwtOptions skewed = strict;
	skewed.clock_skew = std::chrono::seconds{60};
	auto accepted = conflux::http::jwt_decode(token, skewed);
	REQUIRE(accepted.has_value());
	CHECK(accepted->sub == "u");
}

TEST_CASE(
	"jwt: negative registered time claims are rejected",
	"[jwt][auth]") {
	std::string const secret = "session-jwt-secret-32bytes";
	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;

	auto exp = conflux::http::jwt_decode(conflux::http::jwt_sign(R"({"sub":"u","exp":-1})", secret), opts);
	REQUIRE_FALSE(exp.has_value());
	CHECK(exp.error() == "invalid exp claim");

	auto nbf = conflux::http::jwt_decode(conflux::http::jwt_sign(R"({"sub":"u","nbf":-1})", secret), opts);
	REQUIRE_FALSE(nbf.has_value());
	CHECK(nbf.error() == "invalid nbf claim");

	auto iat = conflux::http::jwt_decode(conflux::http::jwt_sign(R"({"sub":"u","iat":-1})", secret), opts);
	REQUIRE_FALSE(iat.has_value());
	CHECK(iat.error() == "invalid iat claim");
}

TEST_CASE(
	"jwt: adversarial malformed base64url segments are rejected",
	"[jwt][auth][security]") {
	std::string const secret = "adversarial-jwt-secret-32bytes";
	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;

	SECTION("header") {
		auto result = conflux::http::jwt_decode("###.eyJzdWIiOiJ1In0.ZmFrZQ", opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "invalid header encoding");
	}
	SECTION("payload") {
		auto result = conflux::http::jwt_decode("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.###.ZmFrZQ", opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "invalid payload encoding");
	}
	SECTION("signature") {
		auto result = conflux::http::jwt_decode("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJ1In0.###", opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "invalid signature encoding");
	}
}

TEST_CASE(
	"jwt: adversarial algorithm confusion attempts are rejected",
	"[jwt][auth][security]") {
	std::string const secret = "adversarial-jwt-secret-32bytes";
	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;

	SECTION("alg none with valid HMAC is still rejected") {
		auto token = conflux::http::jwt_sign(R"({"alg":"none","typ":"JWT"})", R"({"sub":"u"})", secret);
		auto result = conflux::http::jwt_decode(token, opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "unsupported algorithm (only HS256 supported)");
	}
	SECTION("RS256 header with valid HMAC is still rejected") {
		auto token = conflux::http::jwt_sign(R"({"alg":"RS256","typ":"JWT"})", R"({"sub":"u"})", secret);
		auto result = conflux::http::jwt_decode(token, opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "unsupported algorithm (only HS256 supported)");
	}
	SECTION("duplicate alg header is rejected before choosing either value") {
		auto token = conflux::http::jwt_sign(R"({"alg":"HS256","alg":"none","typ":"JWT"})", R"({"sub":"u"})", secret);
		auto result = conflux::http::jwt_decode(token, opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "duplicate alg claim");
	}
}

TEST_CASE(
	"jwt: adversarial duplicate registered claims are rejected",
	"[jwt][auth][security]") {
	std::string const secret = "adversarial-jwt-secret-32bytes";
	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;

	SECTION("duplicate subject") {
		auto token = conflux::http::jwt_sign(R"({"sub":"victim","sub":"attacker"})", secret);
		auto result = conflux::http::jwt_decode(token, opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "duplicate sub claim");
	}
	SECTION("duplicate expiration") {
		auto const now = jwt_test_now();
		auto token = conflux::http::jwt_sign(std::format(R"({{"sub":"u","exp":{},"exp":1}})", now + 3600), secret);
		auto result = conflux::http::jwt_decode(token, opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "duplicate exp claim");
	}
	SECTION("nested object keys with the same spelling are not treated as duplicate top-level claims") {
		auto token = conflux::http::jwt_sign(R"({"sub":"u","nested":{"sub":"inner"}})", secret);
		auto result = conflux::http::jwt_decode(token, opts);
		REQUIRE(result.has_value());
		CHECK(result->sub == "u");
	}
}

TEST_CASE(
	"jwt: registered claims use strict JSON decoding",
	"[jwt][auth][security]") {
	std::string const secret = "strict-json-jwt-secret-32bytes";
	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;

	auto decoded = conflux::http::jwt_decode(
		conflux::http::jwt_sign(R"({"sub":"user\u0031","iss":"issuer\u002dA"})", secret),
		opts);
	REQUIRE(decoded.has_value());
	CHECK(decoded->sub == "user1");
	CHECK(decoded->iss == "issuer-A");

	opts.audience = "api-v1";
	auto audience =
		conflux::http::jwt_decode(conflux::http::jwt_sign(R"({"sub":"u","aud":["api\u002dv1"]})", secret), opts);
	REQUIRE(audience.has_value());
}

TEST_CASE(
	"jwt: unicode-escaped duplicate registered claim is rejected",
	"[jwt][auth][security]") {
	std::string const secret = "strict-json-jwt-secret-32bytes";
	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;

	auto result =
		conflux::http::jwt_decode(conflux::http::jwt_sign(R"({"sub":"victim","\u0073ub":"attacker"})", secret), opts);
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error() == "duplicate sub claim");
}

TEST_CASE(
	"jwt: adversarial missing and huge timestamp claims are rejected by strict policy",
	"[jwt][auth][security]") {
	std::string const secret = "adversarial-jwt-secret-32bytes";
	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;
	opts.require_exp = true;
	opts.require_iat = true;
	opts.max_token_lifetime = std::chrono::minutes{5};

	SECTION("missing exp") {
		auto result = conflux::http::jwt_decode(
			conflux::http::jwt_sign(std::format(R"({{"sub":"u","iat":{}}})", jwt_test_now()), secret),
			opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "missing exp claim");
	}
	SECTION("missing iat") {
		auto result = conflux::http::jwt_decode(
			conflux::http::jwt_sign(std::format(R"({{"sub":"u","exp":{}}})", jwt_test_now() + 60), secret),
			opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "missing iat claim");
	}
	SECTION("out-of-range exp") {
		auto result = conflux::http::jwt_decode(
			conflux::http::jwt_sign(R"({"sub":"u","iat":1,"exp":999999999999999999999999999999})", secret),
			opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "invalid exp claim");
	}
	SECTION("out-of-range iat") {
		auto result = conflux::http::jwt_decode(
			conflux::http::jwt_sign(R"({"sub":"u","iat":999999999999999999999999999999,"exp":9999999999})", secret),
			opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "invalid iat claim");
	}
	SECTION("future lifetime exceeds cap") {
		auto const now = jwt_test_now();
		auto token =
			conflux::http::jwt_sign(std::format(R"({{"sub":"u","iat":{},"exp":{}}})", now, now + 86400), secret);
		auto result = conflux::http::jwt_decode(token, opts);
		REQUIRE_FALSE(result.has_value());
		CHECK(result.error() == "token lifetime too long");
	}
}

TEST_CASE(
	"jwt: oversized bearer tokens are bounded before decode work",
	"[jwt][auth][security]") {
	std::string const secret = "adversarial-jwt-secret-32bytes";
	std::string const payload = std::format(R"({{"sub":"u","blob":"{}"}})", std::string(2048, 'x'));
	auto token = conflux::http::jwt_sign(payload, secret);

	conflux::http::JwtOptions opts;
	opts.secrets = single_secret_rotation(secret);
	opts.verify_exp = false;
	opts.verify_nbf = false;
	opts.max_token_bytes = 512;

	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error() == "token too large");
}
