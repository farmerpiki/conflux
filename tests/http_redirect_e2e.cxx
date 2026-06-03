#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.redirect;
import conflux.net.router;
import conflux.tests.support;

using conflux::http::Config;
using namespace conflux::tests;

namespace {

std::string_view response_body(
	std::string_view response) {
	auto body_start = response.find("\r\n\r\n");
	REQUIRE(body_start != std::string_view::npos);
	return response.substr(body_start + 4);
}

std::uint16_t g_redirect_port = 0;
void ensure_redirect_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		router.use(
			conflux::http::redirect_middleware({
				.rules = {
						  {.from = "/old", .to = "/new", .status = 301},
						  {.from = "/api/v1/", .to = "/api/v2/", .status = 302, .prefix_match = true},
						  }
        }));
		router.get("/new", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("new"); });
		router.get("/api/v2/users", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("v2-users");
		});
		g_redirect_port = test_servers().start(cfg, std::move(router));
	});
}

} // namespace

TEST_CASE(
	"redirect: exact match returns 301 with Location") {
	ensure_redirect_server();
	auto resp = http_get_on(g_redirect_port, "/old");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	REQUIRE(resp.find("Location: /new\r\n") != std::string::npos);
}

TEST_CASE(
	"redirect: prefix match appends suffix and returns 302") {
	ensure_redirect_server();
	auto resp = http_get_on(g_redirect_port, "/api/v1/users");
	REQUIRE(resp.starts_with("HTTP/1.1 302"));
	REQUIRE(resp.find("Location: /api/v2/users\r\n") != std::string::npos);
}

TEST_CASE(
	"redirect: non-matching path passes through to handler") {
	ensure_redirect_server();
	auto resp = http_get_on(g_redirect_port, "/new");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "new");
}

TEST_CASE(
	"redirect: custom status 307 preserved in redirect response") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		router.use(conflux::http::redirect_middleware({.rules = {{.from = "/x", .to = "/y", .status = 307}}}));
		router.get("/y", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("y"); });
		port = test_servers().start(cfg, std::move(router));
	});
	auto resp = http_get_on(port, "/x");
	REQUIRE(resp.starts_with("HTTP/1.1 307"));
	REQUIRE(resp.find("Location: /y\r\n") != std::string::npos);
}
