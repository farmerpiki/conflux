#include <catch2/catch_test_macros.hpp>

#if CONFLUX_HAS_TLS
import std;
import conflux.crypto;
import conflux.net.config;
import conflux.net.jwt;
import conflux.net.router;
import conflux.tests.support;

using conflux::http::Config;
using conflux::http::single_secret_rotation;
using namespace conflux::tests;

namespace {

std::uint16_t g_jwt_port = 0;
std::string g_jwt_secret = "test-secret-key-32bytes";
void ensure_jwt_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		router.use(
			conflux::http::jwt_middleware(conflux::http::JwtOptions{.secrets = single_secret_rotation(g_jwt_secret)}));
		router.get("/api/protected", [](conflux::http::OwnedRequest const &req) {
			auto sub = req.params["jwt_sub"];
			return conflux::http::Response::json(std::format(R"({{"sub":"{}"}})", sub));
		});
		g_jwt_port = test_servers().start(cfg, std::move(router));
	});
}
std::string make_jwt(
	std::string_view payload_json) {
	return conflux::http::jwt_sign_unchecked(payload_json, g_jwt_secret);
}
std::string make_jwt_with_header(
	std::string_view header_json,
	std::string_view payload_json,
	std::string_view secret) {
	auto header_b64 = conflux::crypto::base64url_encode(
		std::span{reinterpret_cast<unsigned char const *>(header_json.data()), header_json.size()});
	auto payload_b64 = conflux::crypto::base64url_encode(
		std::span{reinterpret_cast<unsigned char const *>(payload_json.data()), payload_json.size()});
	std::string const signing_input = header_b64 + '.' + payload_b64;
	auto sig = conflux::crypto::hmac_sha256(
		std::span{reinterpret_cast<unsigned char const *>(secret.data()), secret.size()},
		std::span{reinterpret_cast<unsigned char const *>(signing_input.data()), signing_input.size()});
	auto sig_b64 = conflux::crypto::base64url_encode(std::span{sig.data(), sig.size()});
	return signing_input + '.' + sig_b64;
}

} // namespace
TEST_CASE(
	"jwt: valid token returns 200 and injects sub claim") {
	ensure_jwt_server();
	auto now =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	auto token = make_jwt(std::format(R"({{"sub":"user42","exp":{}}})", now + 3600));
	auto resp = http_get_on(g_jwt_port, "/api/protected", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == R"({"sub":"user42"})");
}
TEST_CASE(
	"jwt: missing Authorization header returns 401") {
	ensure_jwt_server();
	auto resp = http_get_on(g_jwt_port, "/api/protected");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"jwt: wrong secret returns 401") {
	ensure_jwt_server();
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"bad","exp":9999999999})", "wrong-secret");
	auto resp = http_get_on(g_jwt_port, "/api/protected", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"jwt: expired token returns 401") {
	ensure_jwt_server();
	auto token = make_jwt(R"({"sub":"x","exp":1})"); // exp = 1970
	auto resp = http_get_on(g_jwt_port, "/api/protected", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"jwt: malformed token returns 401") {
	ensure_jwt_server();
	auto resp = http_get_on(g_jwt_port, "/api/protected", "Authorization: Bearer not.a.jwt\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"jwt: lowercase bearer scheme returns 200") {
	ensure_jwt_server();
	auto token = make_jwt(R"({"sub":"user42","exp":9999999999})");
	auto resp = http_get_on(g_jwt_port, "/api/protected", std::format("Authorization: bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"jwt_middleware: injected claim params override route params") {
	Config const cfg{.port = 0, .rings = 1};
	conflux::http::Router router;
	router.use(
		conflux::http::jwt_middleware(
			conflux::http::JwtOptions{.secrets = single_secret_rotation("sec", 3), .verify_exp = false}));
	router.get("/api/protected/{jwt_sub}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::json(std::format(R"({{"sub":"{}"}})", req.params["jwt_sub"]));
	});
	auto port = test_servers().start(cfg, std::move(router));
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"victim"})", "sec");
	auto resp = http_get_on(port, "/api/protected/attacker", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == R"({"sub":"victim"})");
}
TEST_CASE(
	"jwt_decode: valid token with no exp returns claims") {
	conflux::http::JwtOptions const opts{.secrets = single_secret_rotation("sec", 3), .verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"alice","iss":"test"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->sub == "alice");
	REQUIRE(result->iss == "test");
}
TEST_CASE(
	"jwt_decode: issuer mismatch returns error") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.issuer = "expected",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","iss":"other"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("issuer") != std::string::npos);
}
TEST_CASE(
	"jwt_decode: audience std::string match") {
	conflux::http::JwtOptions opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "myapp",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"u","aud":"myapp"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
}
TEST_CASE(
	"jwt_decode: audience mismatch returns error") {
	conflux::http::JwtOptions opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "myapp",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"u","aud":"other"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("audience") != std::string::npos);
}
TEST_CASE(
	"jwt_decode: audience A match") {
	conflux::http::JwtOptions opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "myapp",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"u","aud":["svc","myapp"]})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
}
TEST_CASE(
	"jwt_decode: accepts JSON whitespace around claim separators") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "myapp",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(
		"{\n\t\"sub\"\t:\t\"u\",\r\n\t\"aud\"\n:\n[\n\t\"svc\",\r\n\t\"myapp\"\n]\n}",
		"sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->sub == "u");
}
TEST_CASE(
	"jwt_decode: audience A matches only aud claim") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "expected",
		.verify_exp = false};
	auto good = conflux::http::jwt_sign_unchecked(R"({"sub":"x","aud":["other","expected"]})", "sec");
	auto bad = conflux::http::jwt_sign_unchecked(R"({"sub":"expected","aud":["other"]})", "sec");
	REQUIRE(conflux::http::jwt_decode(good, opts).has_value());
	auto result = conflux::http::jwt_decode(bad, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "audience mismatch");
}
TEST_CASE(
	"jwt_decode: malformed audience array is rejected as invalid JSON") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "expected",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","aud":[,"expected"]})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().starts_with("invalid payload JSON:"));
}
TEST_CASE(
	"jwt_decode: malformed header JSON is rejected") {
	conflux::http::JwtOptions const opts{.secrets = single_secret_rotation("sec", 3), .verify_exp = false};
	auto token = make_jwt_with_header(R"({"alg":"HS256)", R"({"sub":"x"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().starts_with("invalid header JSON:"));
}
TEST_CASE(
	"jwt_decode: malformed numeric claims are rejected") {
	conflux::http::JwtOptions const opts{.secrets = single_secret_rotation("sec", 3)};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","exp":"soon"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "invalid exp claim");
}
TEST_CASE(
	"jwt_decode: malformed nbf claim is rejected") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.verify_exp = false,
		.verify_nbf = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","nbf":"later"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "invalid nbf claim");
}
TEST_CASE(
	"jwt_decode: malformed iat claim is rejected") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.verify_exp = false,
		.verify_nbf = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","iat":"earlier"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "invalid iat claim");
}
TEST_CASE(
	"jwt_decode: exp equal to now is expired") {
	auto now =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	conflux::http::JwtOptions const opts{.secrets = single_secret_rotation("sec", 3)};
	auto token = conflux::http::jwt_sign_unchecked(std::format(R"({{"sub":"x","exp":{}}})", now), "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "token expired");
}
TEST_CASE(
	"jwt_decode: nbf in future returns not-yet-valid error") {
	auto far_future =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
		+ 9999;
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.verify_exp = false,
		.verify_nbf = true};
	auto token = conflux::http::jwt_sign_unchecked(std::format(R"({{"sub":"x","nbf":{}}})", far_future), "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("not yet valid") != std::string::npos);
}
TEST_CASE(
	"jwt_decode: nbf in past is accepted") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.verify_exp = false,
		.verify_nbf = true};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","nbf":1})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
}
TEST_CASE(
	"jwt_decode: verify_exp=false allows expired token") {
	conflux::http::JwtOptions const opts{.secrets = single_secret_rotation("sec", 3), .verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","exp":1})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->sub == "x");
}
TEST_CASE(
	"jwt_decode: non-HS256 algorithm returns unsupported error") {
	// Pre-computed base64url (no padding):
	// {"alg":"RS256","typ":"JWT"} → eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9
	// {"sub":"x"}                 → eyJzdWIiOiJ4In0
	std::string_view token = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJ4In0.ZmFrZXNpZw";
	conflux::http::JwtOptions opts{.secrets = single_secret_rotation("sec", 3)};
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("HS256") != std::string::npos);
}
#endif
