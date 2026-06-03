#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.csrf;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

std::uint16_t g_csrf_port = 0;

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

std::string extract_set_cookie(
	std::string_view resp,
	std::string_view name) {
	std::string const needle = std::string{"Set-Cookie: "} + std::string{name} + "=";
	auto pos = resp.find(needle);
	if (pos == std::string_view::npos) {
		return {};
	}
	pos += needle.size();
	auto end = resp.find_first_of(";\r\n", pos);
	return std::string{resp.substr(pos, end - pos)};
}

void ensure_csrf_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::csrf_middleware());
		router.get("/page", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::html("<form>");
		});
		router.post("/submit", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		g_csrf_port = start_mw_server(mw_config(), std::move(router));
	});
}

} // namespace

TEST_CASE(
	"csrf: GET sets csrf_token cookie and X-CSRF-Token response header") {
	ensure_csrf_server();
	auto resp = http_get_on(g_csrf_port, "/page");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Set-Cookie: csrf_token=") != std::string::npos);
	REQUIRE(resp.find("X-CSRF-Token: ") != std::string::npos);
}

TEST_CASE(
	"csrf: POST without cookie returns 403") {
	ensure_csrf_server();
	auto resp = http_post_on(g_csrf_port, "/submit", "application/x-www-form-urlencoded", "x=1", "");
	REQUIRE(resp.starts_with("HTTP/1.1 403"));
}

TEST_CASE(
	"csrf: POST with mismatched token returns 403") {
	ensure_csrf_server();
	auto resp = http_post_on(
		g_csrf_port,
		"/submit",
		"application/x-www-form-urlencoded",
		"x=1",
		"Cookie: csrf_token=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n"
		"X-CSRF-Token: BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 403"));
}

TEST_CASE(
	"csrf: POST with matching cookie and header returns 200") {
	ensure_csrf_server();
	auto get_resp = http_get_on(g_csrf_port, "/page");
	auto token = extract_set_cookie(get_resp, "csrf_token");
	REQUIRE(!token.empty());
	auto post_resp = http_post_on(
		g_csrf_port,
		"/submit",
		"application/x-www-form-urlencoded",
		"x=1",
		std::format("Cookie: csrf_token={}\r\nX-CSRF-Token: {}\r\n", token, token));
	REQUIRE(post_resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(post_resp) == "ok");
}

TEST_CASE(
	"csrf: POST with token in form field (no header) returns 200") {
	ensure_csrf_server();
	auto get_resp = http_get_on(g_csrf_port, "/page");
	auto token = extract_set_cookie(get_resp, "csrf_token");
	REQUIRE(!token.empty());
	auto post_resp = http_post_on(
		g_csrf_port,
		"/submit",
		"application/x-www-form-urlencoded",
		std::format("csrf_token={}", token),
		std::format("Cookie: csrf_token={}\r\n", token));
	REQUIRE(post_resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(post_resp) == "ok");
}

TEST_CASE(
	"csrf: DELETE without token returns 403") {
	ensure_csrf_server();
	auto resp = conflux::tests::http_request_on(g_csrf_port, "DELETE", "/submit", "", "", "");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
}

TEST_CASE(
	"csrf: custom protected_methods excludes DELETE") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::csrf_middleware({.protected_methods = {"POST"}}));
		router.get("/page", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::html("<form>");
		});
		router.del("/resource", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("deleted");
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = conflux::tests::http_request_on(port, "DELETE", "/resource", "", "", "");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "deleted");
}
