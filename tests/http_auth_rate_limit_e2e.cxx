#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.auth;
import conflux.net.config;
import conflux.net.rate_limit;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

std::uint16_t g_auth_port = 0;

void ensure_auth_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::basic_auth_middleware([](std::string_view u, std::string_view p) {
			return u == "testuser" && p == "testpass";
		}));
		router.get("/protected", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("secret");
		});
		g_auth_port = start_mw_server(mw_config(), std::move(router));
	});
}

std::uint16_t g_bearer_port = 0;

void ensure_bearer_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::bearer_auth_middleware([](std::string_view token) { return token == "valid-token-123"; }));
		router.get("/protected", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("secret");
		});
		g_bearer_port = start_mw_server(mw_config(), std::move(router));
	});
}

std::uint16_t g_rate_port = 0;
std::uint16_t g_rate_burst_port = 0;
std::uint16_t g_rate_zero_clients_port = 0;

void ensure_rate_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::rate_limit_middleware({.requests = 2, .window = std::chrono::seconds{60}}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		g_rate_port = start_mw_server(mw_config(), std::move(router));
	});
}

void ensure_rate_burst_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::rate_limit_middleware({.requests = 1, .window = std::chrono::seconds{60}, .burst = 2}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		g_rate_burst_port = start_mw_server(mw_config(), std::move(router));
	});
}

void ensure_rate_zero_clients_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::rate_limit_middleware(
				{.requests = 1, .window = std::chrono::seconds{60}, .max_clients = 0}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		g_rate_zero_clients_port = start_mw_server(mw_config(), std::move(router));
	});
}

std::string extract_header(
	std::string_view resp,
	std::string_view name) {
	auto needle = std::string{name} + ": ";
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

} // namespace

TEST_CASE(
	"basic_auth: missing Authorization returns 401 with WWW-Authenticate") {
	ensure_auth_server();
	auto resp = http_get_on(g_auth_port, "/protected");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find("WWW-Authenticate: Basic") != std::string::npos);
}

TEST_CASE(
	"basic_auth: wrong credentials return 401") {
	ensure_auth_server();
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: Basic YmFkdXNlcjpiYWRwYXNz\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}

TEST_CASE(
	"basic_auth: correct credentials return 200") {
	ensure_auth_server();
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: Basic dGVzdHVzZXI6dGVzdHBhc3M=\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "secret");
}

TEST_CASE(
	"basic_auth: base64 credential without colon returns 401") {
	ensure_auth_server();
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: Basic bm9jb2xvbg==\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}

TEST_CASE(
	"basic_auth: custom realm appears in WWW-Authenticate header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::basic_auth_middleware([](std::string_view, std::string_view) { return false; }, "My Realm"));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find(R"(WWW-Authenticate: Basic realm="My Realm")") != std::string::npos);
}

TEST_CASE(
	"basic_auth: zero max_failed_clients clamps instead of corrupting limiter state",
	"[auth][security]") {
	conflux::http::Router router;
	unsigned calls = 0;
	router.use(
		conflux::http::basic_auth_middleware(
			[&calls](std::string_view, std::string_view) {
				++calls;
				return false;
			},
			conflux::http::BasicAuthOptions{
				.realm = "Clamp",
				.failed_attempts = 1,
				.failed_window = std::chrono::seconds{60},
				.max_failed_clients = 0,
			}));
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/";
	req.remote_addr = "127.0.0.1";
	req.headers.emplace_back("Authorization", "Basic YmFkOmNyZWRz");

	auto first = router.dispatch(req);
	REQUIRE(first.status == 401);
	CHECK(calls == 1);

	auto second = router.dispatch(req);
	REQUIRE(second.status == 429);
	CHECK(calls == 1);
}

TEST_CASE(
	"basic_auth: failed-attempt limit rejects before validating credentials",
	"[auth][security]") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	static std::atomic<unsigned> calls{0};
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::basic_auth_middleware(
				[](std::string_view, std::string_view) {
					++calls;
					return false;
				},
				conflux::http::BasicAuthOptions{
					.realm = "Limited",
					.failed_attempts = 1,
					.failed_window = std::chrono::seconds{60},
					.max_failed_clients = 8,
				}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });
		port = start_mw_server(mw_config(), std::move(router));
	});

	auto first = http_get_on(port, "/", "Authorization: Basic YmFkOmNyZWRz\r\n");
	REQUIRE(first.starts_with("HTTP/1.1 401"));
	auto const before = calls.load();
	auto second = http_get_on(port, "/", "Authorization: Basic YmFkOmNyZWRz\r\n");
	REQUIRE(second.starts_with("HTTP/1.1 429"));
	CHECK(calls.load() == before);
}

TEST_CASE(
	"basic_auth: challenges do not poison limiter before valid credentials",
	"[auth][security]") {
	conflux::http::Router router;
	unsigned calls = 0;
	router.use(
		conflux::http::basic_auth_middleware(
			[&calls](std::string_view u, std::string_view p) {
				++calls;
				return u == "testuser" && p == "testpass";
			},
			conflux::http::BasicAuthOptions{
				.realm = "Limited",
				.failed_attempts = 1,
				.failed_window = std::chrono::seconds{60},
			}));
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/";
	req.remote_addr = "127.0.0.1";

	CHECK(router.dispatch(req).status == 401);
	CHECK(router.dispatch(req).status == 401);
	req.headers.set("Authorization", "Basic dGVzdHVzZXI6dGVzdHBhc3M=");
	auto valid = router.dispatch(req);
	CHECK(valid.status == conflux::http::kHttpOk);
	CHECK(calls == 1);
}

TEST_CASE(
	"basic_auth: valid credentials are gated while failed-attempt bucket is saturated",
	"[auth][security]") {
	conflux::http::Router router;
	unsigned calls = 0;
	router.use(
		conflux::http::basic_auth_middleware(
			[&calls](std::string_view u, std::string_view p) {
				++calls;
				return u == "testuser" && p == "testpass";
			},
			conflux::http::BasicAuthOptions{
				.realm = "Limited",
				.failed_attempts = 1,
				.failed_window = std::chrono::seconds{60},
			}));
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/";
	req.remote_addr = "127.0.0.1";
	req.headers.set("Authorization", "Basic YmFkOmNyZWRz");

	CHECK(router.dispatch(req).status == 401);
	req.headers.set("Authorization", "Basic dGVzdHVzZXI6dGVzdHBhc3M=");
	CHECK(router.dispatch(req).status == 429);
	CHECK(calls == 1);
}

TEST_CASE(
	"basic_auth: valid credentials clear unsaturated failed-attempt bucket",
	"[auth][security]") {
	conflux::http::Router router;
	unsigned calls = 0;
	router.use(
		conflux::http::basic_auth_middleware(
			[&calls](std::string_view u, std::string_view p) {
				++calls;
				return u == "testuser" && p == "testpass";
			},
			conflux::http::BasicAuthOptions{
				.realm = "Limited",
				.failed_attempts = 2,
				.failed_window = std::chrono::seconds{60},
			}));
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/";
	req.remote_addr = "127.0.0.1";
	req.headers.set("Authorization", "Basic YmFkOmNyZWRz");

	CHECK(router.dispatch(req).status == 401);
	req.headers.set("Authorization", "Basic dGVzdHVzZXI6dGVzdHBhc3M=");
	CHECK(router.dispatch(req).status == conflux::http::kHttpOk);
	req.headers.set("Authorization", "Basic YmFkOmNyZWRz");
	CHECK(router.dispatch(req).status == 401);
	CHECK(router.dispatch(req).status == 401);
	CHECK(calls == 4);
}

TEST_CASE(
	"basic_auth: lowercase scheme returns 200") {
	ensure_auth_server();
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: basic dGVzdHVzZXI6dGVzdHBhc3M=\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}

TEST_CASE(
	"bearer_auth: missing Authorization returns 401 with WWW-Authenticate") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find("WWW-Authenticate: Bearer") != std::string::npos);
}

TEST_CASE(
	"bearer_auth: invalid token returns 401") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected", "Authorization: Bearer wrong-token\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}

TEST_CASE(
	"bearer_auth: valid token returns 200") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected", "Authorization: Bearer valid-token-123\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "secret");
}

TEST_CASE(
	"bearer_auth: token with surrounding whitespace is trimmed and accepted") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected", "Authorization: Bearer  valid-token-123 \r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}

TEST_CASE(
	"bearer_auth: lowercase scheme returns 200") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected", "Authorization: bearer valid-token-123\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}

TEST_CASE(
	"rate_limit: first two requests succeed, third returns 429") {
	ensure_rate_server();
	auto r1 = http_get_on(g_rate_port, "/");
	auto r2 = http_get_on(g_rate_port, "/");
	auto r3 = http_get_on(g_rate_port, "/");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r3.starts_with("HTTP/1.1 429"));
	REQUIRE(r3.find("Retry-After:") != std::string::npos);
}

TEST_CASE(
	"rate_limit: burst allows extra requests beyond base rate") {
	ensure_rate_burst_server();
	auto r1 = http_get_on(g_rate_burst_port, "/");
	auto r2 = http_get_on(g_rate_burst_port, "/");
	auto r3 = http_get_on(g_rate_burst_port, "/");
	auto r4 = http_get_on(g_rate_burst_port, "/");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r3.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r4.starts_with("HTTP/1.1 429"));
}

TEST_CASE(
	"rate_limit: 429 response includes Retry-After header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::rate_limit_middleware({.requests = 1, .window = std::chrono::seconds{10}}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto _ = http_get_on(port, "/");
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 429"));
	auto retry = extract_header(resp, "Retry-After");
	REQUIRE(!retry.empty());
	int retry_val = std::stoi(std::string{retry});
	REQUIRE(retry_val > 0);
	REQUIRE(retry_val <= 10);
}

TEST_CASE(
	"rate_limit: max_clients zero is clamped to one client") {
	ensure_rate_zero_clients_server();
	auto r1 = http_get_on(g_rate_zero_clients_port, "/");
	auto r2 = http_get_on(g_rate_zero_clients_port, "/");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 429"));
}
