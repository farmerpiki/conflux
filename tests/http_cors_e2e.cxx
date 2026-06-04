#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.cors;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

std::uint16_t g_cors_port = 0;

void ensure_cors_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::cors_middleware({.allowed_origins = {"https://test.example"}}));
		router.get("/api", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"ok":true})");
		});
		router.get("/vary", [](conflux::http::OwnedRequest const &) {
			auto r = conflux::http::Response::text("vary");
			r.headers["Vary"] = "Accept-Encoding";
			return r;
		});
		g_cors_port = start_mw_server(mw_config(), std::move(router));
	});
}

std::uint16_t g_cors_cred_port = 0;

void ensure_cors_cred_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::cors_middleware({
				.allowed_origins = {"*"},
				.expose_headers = {"X-Custom-Header", "X-Request-Id"},
				.allow_credentials = true,
        }));
		router.get("/api", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"ok":true})");
		});
		g_cors_cred_port = start_mw_server(mw_config(), std::move(router));
	});
}

std::uint16_t g_cors_wildcard_port = 0;

void ensure_cors_wildcard_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::cors_middleware());
		router.get("/api", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"ok":true})");
		});
		g_cors_wildcard_port = start_mw_server(mw_config(), std::move(router));
	});
}

} // namespace

TEST_CASE(
	"cors: preflight with matching origin returns 204 and ACAO") {
	ensure_cors_server();
	auto resp = http_options_on(
		g_cors_port,
		"/api",
		"Origin: https://test.example\r\n"
		"Access-Control-Request-Method: GET\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 204"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://test.example") != std::string::npos);
	REQUIRE(resp.find("Access-Control-Allow-Methods:") != std::string::npos);
}

TEST_CASE(
	"cors: preflight with non-matching origin has no ACAO") {
	ensure_cors_server();
	auto resp = http_options_on(
		g_cors_port,
		"/api",
		"Origin: https://evil.com\r\n"
		"Access-Control-Request-Method: GET\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 204"));
	REQUIRE(resp.find("Access-Control-Allow-Origin:") == std::string::npos);
}

TEST_CASE(
	"cors: GET with matching origin receives ACAO header") {
	ensure_cors_server();
	auto resp = http_get_on(g_cors_port, "/api", "Origin: https://test.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://test.example") != std::string::npos);
}

TEST_CASE(
	"cors: GET with matching origin appends Origin to existing Vary") {
	ensure_cors_server();
	auto resp = http_get_on(g_cors_port, "/vary", "Origin: https://test.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Vary: Accept-Encoding, Origin") != std::string::npos);
}

TEST_CASE(
	"cors: GET without Origin header has no ACAO header") {
	ensure_cors_server();
	auto resp = http_get_on(g_cors_port, "/api");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin:") == std::string::npos);
}

TEST_CASE(
	"cors: allow_credentials reflects origin instead of wildcard") {
	ensure_cors_cred_server();
	auto resp = http_get_on(g_cors_cred_port, "/api", "Origin: https://foo.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://foo.example") != std::string::npos);
	REQUIRE(resp.find("Access-Control-Allow-Credentials: true") != std::string::npos);
}

TEST_CASE(
	"cors: expose_headers present in non-preflight response") {
	ensure_cors_cred_server();
	auto resp = http_get_on(g_cors_cred_port, "/api", "Origin: https://foo.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Expose-Headers:") != std::string::npos);
	REQUIRE(resp.find("X-Custom-Header") != std::string::npos);
	REQUIRE(resp.find("X-Request-Id") != std::string::npos);
}

TEST_CASE(
	"cors: preflight with allow_credentials reflects origin and sets credentials header") {
	ensure_cors_cred_server();
	auto resp = http_options_on(
		g_cors_cred_port,
		"/api",
		"Origin: https://foo.example\r\n"
		"Access-Control-Request-Method: POST\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 204"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://foo.example") != std::string::npos);
	REQUIRE(resp.find("Access-Control-Allow-Credentials: true") != std::string::npos);
}

TEST_CASE(
	"cors: wildcard origin without credentials returns ACAO: *") {
	ensure_cors_wildcard_server();
	auto resp = http_get_on(g_cors_wildcard_port, "/api", "Origin: https://any.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: *") != std::string::npos);
	REQUIRE(resp.find("Access-Control-Allow-Credentials:") == std::string::npos);
}

TEST_CASE(
	"cors: OPTIONS without Access-Control-Request-Method is not a preflight") {
	ensure_cors_server();
	auto resp = http_options_on(g_cors_port, "/api", "Origin: https://test.example\r\n");
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://test.example") != std::string::npos);
	REQUIRE(resp.find("Access-Control-Allow-Methods:") == std::string::npos);
}
