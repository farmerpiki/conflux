#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.cache_control;
import conflux.net.config;
import conflux.net.forwarded;
import conflux.net.ip_filter;
import conflux.net.router;
import conflux.net.trailing_slash;
import conflux.tests.support;

using conflux::http::Config;
using namespace conflux::tests;

namespace {

std::uint16_t g_fwd_port = 0;
std::uint16_t g_fwd_strict_empty_port = 0;
void ensure_forwarded_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::forwarded_middleware({.trusted_proxies = {"127.0.0.1/32"}}));
		router.get("/addr", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(req.remote_addr);
		});
		g_fwd_port = start_mw_server(mw_config(), std::move(router));
	});
}
void ensure_forwarded_strict_empty_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::forwarded_middleware({}));
		router.get("/addr", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(req.remote_addr);
		});
		g_fwd_strict_empty_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_ipallow_port = 0;
std::uint16_t g_ipblock_port = 0;
void ensure_ipallow_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::ip_filter_middleware({
				.mode = conflux::http::IpFilterMode::allowlist,
				.cidrs = {"127.0.0.0/8"},
			}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		g_ipallow_port = start_mw_server(mw_config(), std::move(router));
	});
}
void ensure_ipblock_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::ip_filter_middleware({
				.mode = conflux::http::IpFilterMode::blocklist,
				.cidrs = {"127.0.0.1/32"},
			}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		g_ipblock_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_ipallow_block_port = 0;
void ensure_ipallow_block_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::ip_filter_middleware({
				.mode = conflux::http::IpFilterMode::allowlist,
				.cidrs = {"192.168.0.0/24"},
			}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		g_ipallow_block_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_ipblock_pass_port = 0;
void ensure_ipblock_pass_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::ip_filter_middleware({
				.mode = conflux::http::IpFilterMode::blocklist,
				.cidrs = {"10.0.0.0/8"},
			}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		g_ipblock_pass_port = start_mw_server(mw_config(), std::move(router));
	});
}

std::uint16_t g_cache_port = 0;
void ensure_cache_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::cache_control_middleware({
				.rules =
					{
							{"image/", "max-age=31536000, immutable"},
							{"text/css", "max-age=86400, public"},
							{"application/json", "no-store"},
							},
				.default_directive = "no-cache",
        }));
		router.get("/image", [](conflux::http::OwnedRequest const &) {
			conflux::http::Response r;
			r.content_type = "image/png";
			r.set_text_body("img");
			return r;
		});
		router.get("/css", [](conflux::http::OwnedRequest const &) {
			conflux::http::Response r;
			r.content_type = "text/css";
			r.set_text_body("body{}");
			return r;
		});
		router.get("/api", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({})"); });
		router.get("/html", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::html("<p>hi</p>");
		});
		router.get("/custom", [](conflux::http::OwnedRequest const &) {
			auto r = conflux::http::Response::text("x");
			r.headers["Cache-Control"] = "max-age=999";
			return r;
		});
		g_cache_port = start_mw_server(mw_config(), std::move(router));
	});
}

std::uint16_t g_ts_remove_port = 0;
std::uint16_t g_ts_add_port = 0;
void ensure_ts_remove_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::trailing_slash_middleware());
		router.get("/foo", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("foo"); });
		g_ts_remove_port = start_mw_server(mw_config(), std::move(router));
	});
}
void ensure_ts_add_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::trailing_slash_middleware({.mode = conflux::http::TrailingSlashMode::add}));
		router.get("/bar/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("bar"); });
		g_ts_add_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_ts_308_port = 0;
void ensure_ts_308_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::trailing_slash_middleware({.redirect_status = 308}));
		router.get("/foo", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("foo"); });
		g_ts_308_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_ts_307_port = 0;
void ensure_ts_307_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::trailing_slash_middleware({.redirect_status = 307}));
		router.get("/foo", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("foo"); });
		g_ts_307_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::string extract_header(
	std::string_view resp,
	std::string_view name) {
	auto needle = std::string{"\r\n"} + std::string{name} + ": ";
	auto pos = resp.find(needle);
	if (pos == std::string_view::npos) {
		return {};
	}
	pos += needle.size();
	auto end = resp.find("\r\n", pos);
	if (end == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos, end - pos)};
}
std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

} // namespace

// conflux::http::forwarded_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"forwarded: X-Forwarded-For from trusted proxy rewrites remote_addr") {
	ensure_forwarded_server();
	auto resp = http_get_on(g_fwd_port, "/addr", "X-Forwarded-For: 203.0.113.5\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "203.0.113.5");
}
TEST_CASE(
	"forwarded: X-Real-IP from trusted proxy rewrites remote_addr") {
	ensure_forwarded_server();
	auto resp = http_get_on(g_fwd_port, "/addr", "X-Real-IP: 198.51.100.7\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "198.51.100.7");
}
TEST_CASE(
	"forwarded: X-Forwarded-For chain uses leftmost entry") {
	ensure_forwarded_server();
	auto resp = http_get_on(g_fwd_port, "/addr", "X-Forwarded-For: 10.0.0.1, 172.16.0.1\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "10.0.0.1");
}
TEST_CASE(
	"forwarded: no forwarding header keeps original remote_addr") {
	ensure_forwarded_server();
	auto resp = http_get_on(g_fwd_port, "/addr");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	// peer is 127.0.0.1 (loopback), no XFF header — keep as-is
	REQUIRE(resp.substr(hdr_end + 4) == "127.0.0.1");
}
TEST_CASE(
	"forwarded: strict mode with empty trusted_proxies ignores X-Forwarded-For") {
	ensure_forwarded_strict_empty_server();
	auto resp = http_get_on(g_fwd_strict_empty_port, "/addr", "X-Forwarded-For: 203.0.113.9\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	// Default is strict: empty trust list trusts nobody, remote_addr stays loopback.
	REQUIRE(resp.substr(hdr_end + 4) == "127.0.0.1");
}
TEST_CASE(
	"forwarded: strict mode with empty trusted_proxies ignores X-Real-IP") {
	ensure_forwarded_strict_empty_server();
	auto resp = http_get_on(g_fwd_strict_empty_port, "/addr", "X-Real-IP: 198.51.100.99\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "127.0.0.1");
}
TEST_CASE(
	"forwarded: empty trusted_proxies has no trust-all fallback") {
	ensure_forwarded_strict_empty_server();
	auto resp = http_get_on(g_fwd_strict_empty_port, "/addr", "X-Forwarded-For: 203.0.113.9\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "127.0.0.1");
}
TEST_CASE(
	"forwarded: use_x_forwarded_for=false falls back to X-Real-IP") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::forwarded_middleware({
				.trusted_proxies = {"127.0.0.1/32"},
				.use_x_forwarded_for = false,
				.use_x_real_ip = true,
			}));
		router.get("/addr", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.remote_addr});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	// X-Forwarded-For is set but should be ignored; X-Real-IP wins.
	auto resp = http_get_on(
		port,
		"/addr",
		"X-Forwarded-For: 1.2.3.4\r\n"
		"X-Real-IP: 5.6.7.8\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "5.6.7.8");
}
TEST_CASE(
	"forwarded: untrusted peer has X-Forwarded-For stripped before downstream") {
	ScopedTestServer srv{mw_config(), [] {
							 conflux::http::Router r;
							 r.use(conflux::http::forwarded_middleware({})); // strict: no trusted proxies
							 // Echo the header as-seen by the downstream handler.
							 r.get("/xff", [](conflux::http::OwnedRequest const &req) {
								 return conflux::http::Response::text(std::string{req.headers["x-forwarded-for"]});
							 });
							 return r;
						 }()};
	// Send a spoofed X-Forwarded-For from an untrusted peer (loopback).
	auto resp = http_get_on(srv.port(), "/xff", "X-Forwarded-For: 203.0.113.99\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Middleware must strip the header; downstream sees empty S.
	REQUIRE(extract_body(resp).empty());
}
// ---------------------------------------------------------------------------
// conflux::http::ip_filter_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"ip_filter: allowlist passes loopback request") {
	ensure_ipallow_server();
	auto resp = http_get_on(g_ipallow_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"ip_filter: blocklist blocks loopback request") {
	ensure_ipblock_server();
	auto resp = http_get_on(g_ipblock_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
}
TEST_CASE(
	"ip_filter: allowlist blocks non-matching IP") {
	ensure_ipallow_block_server();
	auto resp = http_get_on(g_ipallow_block_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
}
TEST_CASE(
	"ip_filter: blocklist passes non-matching IP") {
	ensure_ipblock_pass_server();
	auto resp = http_get_on(g_ipblock_pass_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"ip_filter: empty allowlist blocks all requests") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::ip_filter_middleware({.mode = conflux::http::IpFilterMode::allowlist, .cidrs = {}}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
}
// ---------------------------------------------------------------------------
// conflux::http::cache_control_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"cache_control: image/* gets immutable max-age rule") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/image");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Cache-Control: max-age=31536000, immutable") != std::string::npos);
}
TEST_CASE(
	"cache_control: text/css gets its specific rule") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/css");
	REQUIRE(resp.find("Cache-Control: max-age=86400, public") != std::string::npos);
}
TEST_CASE(
	"cache_control: application/json gets no-store") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/api");
	REQUIRE(resp.find("Cache-Control: no-store") != std::string::npos);
}
TEST_CASE(
	"cache_control: unmatched MIME gets default directive") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/html");
	REQUIRE(resp.find("Cache-Control: no-cache") != std::string::npos);
}
TEST_CASE(
	"cache_control: handler-set Cache-Control is not overwritten") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/custom");
	REQUIRE(resp.find("Cache-Control: max-age=999") != std::string::npos);
}
TEST_CASE(
	"cache_control: Content-Type with charset still matches mime prefix") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::cache_control_middleware({
				.rules = {{"text/html", "max-age=60"}},
			}));
		router.get("/", [](conflux::http::OwnedRequest const &) {
			conflux::http::Response r;
			r.content_type = "text/html; charset=utf-8";
			r.set_text_body("<p/>");
			return r;
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.find("Cache-Control: max-age=60") != std::string::npos);
}
TEST_CASE(
	"cache_control: empty mime_prefix rule matches everything") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::cache_control_middleware({
				.rules = {{"image/", "max-age=99999"}, {"", "no-store"}},
        }));
		router.get("/any", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/any");
	REQUIRE(resp.find("Cache-Control: no-store") != std::string::npos);
}
// ---------------------------------------------------------------------------
// conflux::http::trailing_slash_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"trailing_slash: /foo/ redirects to /foo with 301") {
	ensure_ts_remove_server();
	auto resp = http_get_on(g_ts_remove_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	REQUIRE(resp.find("Location: /foo\r\n") != std::string::npos);
}
TEST_CASE(
	"trailing_slash: /foo without slash passes through") {
	ensure_ts_remove_server();
	auto resp = http_get_on(g_ts_remove_port, "/foo");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"trailing_slash: root / is never redirected") {
	ensure_ts_remove_server();
	// Router returns 404 for /, but it must not be a 301.
	auto resp = http_get_on(g_ts_remove_port, "/");
	REQUIRE(!resp.starts_with("HTTP/1.1 301"));
}
TEST_CASE(
	"trailing_slash: add mode redirects /bar to /bar/") {
	ensure_ts_add_server();
	auto resp = http_get_on(g_ts_add_port, "/bar");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	REQUIRE(resp.find("Location: /bar/\r\n") != std::string::npos);
}
TEST_CASE(
	"trailing_slash: add mode passes /bar/ through") {
	ensure_ts_add_server();
	auto resp = http_get_on(g_ts_add_port, "/bar/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"trailing_slash: redirect_status=308 emits 308") {
	ensure_ts_308_server();
	auto resp = http_get_on(g_ts_308_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 308"));
	REQUIRE(resp.find("Location: /foo\r\n") != std::string::npos);
}
TEST_CASE(
	"trailing_slash: redirect_status=307 emits 307 Temporary Redirect") {
	ensure_ts_307_server();
	auto resp = http_get_on(g_ts_307_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 307 Temporary Redirect"));
	REQUIRE(resp.find("Location: /foo\r\n") != std::string::npos);
}
TEST_CASE(
	"trailing_slash: redirect_status=308 emits 308 Permanent Redirect") {
	ensure_ts_308_server();
	auto resp = http_get_on(g_ts_308_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 308 Permanent Redirect"));
}
TEST_CASE(
	"trailing_slash: query std::string is preserved in redirect Location") {
	ensure_ts_remove_server();
	// /foo/?x=1&y=2 should redirect to /foo?x=1&y=2
	auto resp = http_get_on(g_ts_remove_port, "/foo/?x=1&y=2");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	auto loc = extract_header(resp, "Location");
	REQUIRE(loc.starts_with("/foo?"));
	REQUIRE(loc.find("x=1") != std::string::npos);
	REQUIRE(loc.find("y=2") != std::string::npos);
}
TEST_CASE(
	"trailing_slash: query std::string with spaces is percent-encoded in Location") {
	ensure_ts_remove_server();
	// /foo/?name=hello world should percent-encode the space
	auto resp = http_get_on(g_ts_remove_port, "/foo/?name=hello%20world");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	auto loc = extract_header(resp, "Location");
	REQUIRE(loc.find("name=hello%20world") != std::string::npos);
}
