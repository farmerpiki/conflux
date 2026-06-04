#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.router;
import conflux.net.security;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

std::uint16_t g_security_port = 0;

void ensure_security_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::SecurityOptions sopts{};
		sopts.hsts_only_on_tls = false;
		conflux::http::Router router;
		router.use(conflux::http::security_headers_middleware(sopts));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		g_security_port = start_mw_server(mw_config(), std::move(router));
	});
}

} // namespace

TEST_CASE(
	"security: default options inject HSTS header") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("Strict-Transport-Security:") != std::string::npos);
	REQUIRE(resp.find("max-age=") != std::string::npos);
}

TEST_CASE(
	"security: default options inject X-Frame-Options DENY") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("X-Frame-Options: DENY") != std::string::npos);
}

TEST_CASE(
	"security: default options inject X-Content-Type-Options nosniff") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("X-Content-Type-Options: nosniff") != std::string::npos);
}

TEST_CASE(
	"security: default options inject Referrer-Policy") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("Referrer-Policy:") != std::string::npos);
}

TEST_CASE(
	"security: default X-XSS-Protection is 0 (OWASP-recommended)") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("X-XSS-Protection: 0") != std::string::npos);
}

TEST_CASE(
	"security: default options inject Permissions-Policy") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("Permissions-Policy:") != std::string::npos);
}

TEST_CASE(
	"security: custom CSP is injected") {
	auto cfg = mw_config();

	conflux::http::SecurityOptions sopts{};
	sopts.csp = "default-src 'self'";

	conflux::http::Router router;
	router.use(conflux::http::security_headers_middleware(sopts));
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });

	ScopedTestServer srv{cfg, std::move(router)};
	auto resp = http_get_on(srv.port(), "/");
	REQUIRE(resp.find("Content-Security-Policy: default-src 'self'") != std::string::npos);
}

TEST_CASE(
	"security: hsts with no subdomains omits includeSubDomains") {
	auto cfg = mw_config();

	conflux::http::SecurityOptions sopts{};
	sopts.hsts_include_subdomains = false;
	sopts.hsts_only_on_tls = false;

	conflux::http::Router router;
	router.use(conflux::http::security_headers_middleware(sopts));
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });

	ScopedTestServer srv{cfg, std::move(router)};
	auto resp = http_get_on(srv.port(), "/");
	REQUIRE(resp.find("Strict-Transport-Security:") != std::string::npos);
	REQUIRE(resp.find("includeSubDomains") == std::string::npos);
}

TEST_CASE(
	"security: hsts_max_age=0 disables HSTS header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::security_headers_middleware({.hsts_max_age = 0}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.find("Strict-Transport-Security") == std::string::npos);
}

TEST_CASE(
	"security: empty frame_options disables X-Frame-Options header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::security_headers_middleware({.frame_options = ""}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.find("X-Frame-Options") == std::string::npos);
}
