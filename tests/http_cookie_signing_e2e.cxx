#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.cookie_signing;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;
using conflux::http::single_secret_rotation;

namespace {

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

constexpr std::string_view kCookieMiddlewareSecret = "srv-secret-16-bytes";
constexpr std::string_view kOtherCookieSecret = "other-secret-16-bytes";

} // namespace

TEST_CASE(
	"cookie_signing: sign then verify returns original value") {
	auto signed_val = conflux::http::sign_cookie_unchecked("hello", "my-secret");
	REQUIRE(signed_val.starts_with("hello."));
	auto result = conflux::http::verify_cookie_unchecked(signed_val, "my-secret");
	REQUIRE(result.has_value());
	REQUIRE(*result == "hello");
}

TEST_CASE(
	"cookie_signing: verify with wrong secret returns std::nullopt") {
	auto signed_val = conflux::http::sign_cookie_unchecked("hello", "my-secret");
	auto result = conflux::http::verify_cookie_unchecked(signed_val, "wrong-secret");
	REQUIRE(!result.has_value());
}

TEST_CASE(
	"cookie_signing: tampered signature returns std::nullopt") {
	auto signed_val = conflux::http::sign_cookie_unchecked("hello", "my-secret");
	signed_val.back() = (signed_val.back() == 'A') ? 'B' : 'A';
	auto result = conflux::http::verify_cookie_unchecked(signed_val, "my-secret");
	REQUIRE(!result.has_value());
}

TEST_CASE(
	"cookie_signing: value without dot returns std::nullopt") {
	auto result = conflux::http::verify_cookie_unchecked("nodot", "any-secret");
	REQUIRE(!result.has_value());
}

TEST_CASE(
	"cookie_signing: value with dots round-trips correctly") {
	auto signed_val = conflux::http::sign_cookie_unchecked("user.name@host.example", "my-secret");
	auto result = conflux::http::verify_cookie_unchecked(signed_val, "my-secret");
	REQUIRE(result.has_value());
	REQUIRE(*result == "user.name@host.example");
}

TEST_CASE(
	"cookie_signing_middleware: short secret throws std::invalid_argument") {
	REQUIRE_THROWS_AS(
		conflux::http::cookie_signing_middleware({.secrets = single_secret_rotation("tooshort")}),
		std::invalid_argument);
}

TEST_CASE(
	"cookie_signing_middleware: valid signed cookie is unwrapped") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::cookie_signing_middleware(
				{.secrets = single_secret_rotation(std::string{kCookieMiddlewareSecret})}));
		router.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.cookies["session"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto signed_val = conflux::http::sign_cookie_unchecked("user42", kCookieMiddlewareSecret);
	auto resp = http_get_on(port, "/echo", std::format("Cookie: session={}\r\n", signed_val));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "user42");
}

TEST_CASE(
	"cookie_signing_middleware: invalid signature strips cookie value") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::cookie_signing_middleware(
				{.secrets = single_secret_rotation(std::string{kCookieMiddlewareSecret}), .strip_invalid = true}));
		router.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.cookies["session"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto bad_val = conflux::http::sign_cookie_unchecked("attacker", kOtherCookieSecret);
	auto resp = http_get_on(port, "/echo", std::format("Cookie: session={}\r\n", bad_val));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp).empty());
}

TEST_CASE(
	"cookie_signing_middleware: unsigned cookie (no dot) passes through") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::cookie_signing_middleware(
				{.secrets = single_secret_rotation(std::string{kCookieMiddlewareSecret})}));
		router.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.cookies["plain"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/echo", "Cookie: plain=nodot\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "nodot");
}

TEST_CASE(
	"cookie_signing_middleware: strip_invalid=false keeps invalid cookie as-is") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::cookie_signing_middleware(
				{.secrets = single_secret_rotation("srv-secret-key-1234"), .strip_invalid = false}));
		router.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.cookies["session"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto bad_val = conflux::http::sign_cookie_unchecked("user", "wrong-secret-key-1234");
	auto resp = http_get_on(port, "/echo", std::format("Cookie: session={}\r\n", bad_val));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == bad_val);
}
