// Plain TU — intentionally not a module unit.
// std::thread with a lambda inside a module unit triggers GCC's TU-local
// entity exposure rule (the lambda type leaks into Tup instantiations).
// A plain TU has no such restriction.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <conflux/detail/discard.hxx>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#if CONFLUX_HAS_TLS
	#include <openssl/err.h>
	#include <openssl/ssl.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

import std;
import conflux.types;

import conflux.crypto;
import conflux.json;
import conflux.http.extended;
import conflux.net.app;
import conflux.net.auth;
import conflux.net.cache_control;
import conflux.net.compress;
import conflux.net.config;
import conflux.net.cookie_signing;
import conflux.net.cors;
import conflux.net.forwarded;
import conflux.net.http.client;
import conflux.net.http.static_files;
import conflux.net.http_server;
import conflux.net.ip_filter;
import conflux.net.metrics;
import conflux.net.openapi;
import conflux.net.rate_limit;
import conflux.net.redirect;
import conflux.net.router;
import conflux.net.security;
import conflux.net.trailing_slash;
#if CONFLUX_HAS_TLS
import conflux.net.jwt;
#endif
import conflux.net.compress;
import conflux.net.http.static_core;
import conflux.net.http1_parser;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
import conflux.tests.support;
import conflux.work;

using conflux::http::Config;
using conflux::http::ParserLimits;
using conflux::http::single_secret_rotation;
using namespace conflux::json;
using namespace conflux::tests;

namespace {
namespace chttp = conflux::http;
using conflux::http::HttpClient;
using conflux::http::HttpClientOptions;
using conflux::work::WorkPool;
using conflux::work::WorkPoolOptions;

// Actual port chosen by the OS; set once in ensure_server().
std::uint16_t g_test_port = 0;

void ensure_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Config cfg{};
		cfg.port = 0; // let OS pick a free port — no cross-process port races
		cfg.rings = 1;
		cfg.ring_entries = 256;
		cfg.single_issuer = true;
		cfg.defer_taskrun = true;
		cfg.coop_taskrun = true;
		cfg.taskrun_flag = true;

		conflux::http::Router router;
		router.get("/", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::html("<html><body><h1>Hello from conflux!</h1></body></html>");
		});
		router.get("/hello/{name}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::html(
				std::format("<html><body><h1>Hello, {}!</h1></body></html>", req.params["name"]));
		});
		router.get("/api/ping", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"status":"ok"})");
		});
		router.get(
			"/api/task-ping",
			[](conflux::http::OwnedRequest const &) -> conflux::work::root::Task<conflux::http::Response> {
				auto [task, source] = conflux::work::root::make_task_source<conflux::http::Response>();
				(void)source.try_set_value(
					conflux::work::root::Success<conflux::http::Response>{
						conflux::http::Response::json(R"({"task":"ok"})")});
				return std::move(task);
			});
		router.get("/api/echo-header", [](conflux::http::OwnedRequest const &req) {
			auto v = req.headers["x-test-header"];
			if (v.empty()) {
				return conflux::http::Response::not_found("x-test-header");
			}
			return conflux::http::Response::text(std::string{v});
		});
		router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(req.body);
		});
		router.post("/api/echo-json", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(req.body);
		});
		router.get("/api/echo-query", [](conflux::http::OwnedRequest const &req) {
			auto v = req.query["key"];
			if (v.empty()) {
				return conflux::http::Response::not_found("key");
			}
			return conflux::http::Response::text(std::string{v});
		});
		router.get("/api/with-header", [](conflux::http::OwnedRequest const &) {
			auto r = conflux::http::Response::text("ok");
			r.headers["X-Custom"] = "hello";
			r.headers["X-Another"] = "world";
			return r;
		});
		router.get("/api/redirect-302", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::redirect("/api/ping");
		});
		router.get("/api/redirect-301", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::redirect("/api/ping", 301);
		});
		// PUT / PATCH / DELETE / OPTIONS routes.
		router.put("/api/resource/{id}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(std::format(R"({{"method":"PUT","id":"{}"}})", req.params["id"]));
		});
		router.patch("/api/resource/{id}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(std::format(R"({{"method":"PATCH","id":"{}"}})", req.params["id"]));
		});
		router.del("/api/resource/{id}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(std::format(R"({{"method":"DELETE","id":"{}"}})", req.params["id"]));
		});
		router.options("/api/resource", [](conflux::http::OwnedRequest const &) {
			auto r = conflux::http::Response::text("");
			r.status = 204;
			r.status_text = "No Content";
			r.headers["Allow"] = "GET, POST, PUT, PATCH, DELETE, OPTIONS";
			return r;
		});
		// Route group: /api/v2/* with a version header middleware.
		router.group("/api/v2", [](conflux::http::Router::Group &g) {
			g.use([](conflux::http::OwnedRequest const &req, conflux::http::Router::Handler const &next) {
				auto resp = next(req);
				resp.headers["X-Api-Version"] = "2";
				return resp;
			});
			g.get("/status", [](conflux::http::OwnedRequest const &) {
				return conflux::http::Response::json(R"({"v":"2","status":"ok"})");
			});
			g.get("/item/{id}", [](conflux::http::OwnedRequest const &req) {
				return conflux::http::Response::json(std::format(R"({{"id":"{}"}})", req.params["id"]));
			});
		});
		// Route outside the group — must NOT have X-Api-Version header.
		router.get("/api/v1/status", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"v":"1","status":"ok"})");
		});
		// Cookie echo: returns value of named cookie.
		router.get("/api/echo-cookie", [](conflux::http::OwnedRequest const &req) {
			auto v = req.cookies["name"];
			if (v.empty()) {
				return conflux::http::Response::not_found("name");
			}
			return conflux::http::Response::text(std::string{v});
		});
		// Set-cookie: sets two cookies on the response.
		router.get("/api/set-cookie", [](conflux::http::OwnedRequest const &) {
			auto r = conflux::http::Response::text("ok");
			r.set_cookie("session", "abc123", "Path=/; HttpOnly");
			r.set_cookie("theme", "dark");
			return r;
		});
		// Create server on heap so port() can be queried from this std::thread
		// while run() blocks on the worker std::thread.
		g_test_port = test_servers().start(cfg, std::move(router));
	});
}
// Connect, send a GET, parse Content-Length, return the full response.
// No shutdown(SHUT_WR) needed — stops reading once body is complete.
std::string http_get(
	std::string_view path) {
	ensure_server();
	return conflux::tests::http_get_on(g_test_port, path);
}
// Like http_get but sends extra request headers.
std::string http_get_with_headers(
	std::string_view path,
	std::string_view extra_headers) {
	ensure_server();
	return conflux::tests::http_get_on(g_test_port, path, extra_headers);
}
// Send a POST request with a body; returns the full response.
std::string http_post(
	std::string_view path,
	std::string_view content_type,
	std::string_view body) {
	ensure_server();
	return conflux::tests::http_post_on(g_test_port, path, content_type, body);
}
// Send an arbitrary HTTP request with a body.
std::string http_request(
	std::string_view method,
	std::string_view path,
	std::string_view content_type = "",
	std::string_view body = "") {
	ensure_server();
	return conflux::tests::http_request_on(g_test_port, method, path, content_type, body, "Connection: close\r\n");
}
// Read exactly one HTTP/1.1 response from an already-connected fd.
// Returns the full raw response (status + headers + body).
// Send two sequential GET requests on one persistent connection.
// Returns {response1, response2}.
std::pair<std::string, std::string> http_two_gets(
	std::string_view path1,
	std::string_view path2) {
	ensure_server();

	LocalTcpClient client{g_test_port};
	auto r1_str = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\n\r\n", path1);
	(void)client.send(r1_str);
	auto resp1 = client.read_one_response();

	auto r2_str = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\n\r\n", path2);
	(void)client.send(r2_str);
	auto resp2 = client.read_one_response();

	return {resp1, resp2};
}
// Returns true if the server closed the connection after the response.
bool server_closed_after(
	std::string_view path,
	std::string_view extra_headers) {
	ensure_server();

	LocalTcpClient client{g_test_port};
	auto req = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\n{}\r\n", path, extra_headers);
	(void)client.send(req);
	(void)client.read_one_response(); // discard response

	// Try one more recv — should return 0 if server closed.
	char probe{};
	auto n = client.recv(&probe, 1);
	return n == 0;
}
// Alias: GET with a single extra header line (must end with \r\n).
std::string http_get_with_header_on(
	std::uint16_t port,
	std::string_view path,
	std::string_view header) {
	return conflux::tests::http_get_on(port, path, header);
}
// Gzip-decompress a buffer; returns empty on failure.
std::string gzip_decompress(
	std::string_view compressed) {
	z_stream zs{};
	if (inflateInit2(&zs, 15 | 16) != Z_OK) {
		return {};
	}
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-const-cast)
	zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
	zs.avail_in = static_cast<uInt>(compressed.size());
	std::string out;
	std::array<char, 4096> chunk{};
	int rc = Z_OK;
	while (rc == Z_OK) {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
		zs.next_out = reinterpret_cast<Bytef *>(chunk.data());
		zs.avail_out = static_cast<uInt>(chunk.size());
		rc = inflate(&zs, Z_NO_FLUSH);
		out.append(chunk.data(), chunk.size() - zs.avail_out);
	}
	inflateEnd(&zs);
	return rc == Z_STREAM_END ? out : std::string{};
}
// ---------------------------------------------------------------------------
// compress_middleware test server
// ---------------------------------------------------------------------------

std::uint16_t g_compress_port = 0;
void ensure_compress_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::compress_middleware());
		// Large body (>256 bytes) so min_body_size is exceeded.
		router.get("/big", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::html(std::string(512, 'A'));
		});
		// Small body (<256 bytes).
		router.get("/small", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::html("hi"); });
		// Non-compressible MIME type.
		router.get("/bin", [](conflux::http::OwnedRequest const &) {
			conflux::http::Response r;
			r.status = 200;
			r.status_text = "OK";
			r.content_type = "application/octet-stream";
			r.set_text_body(std::string(512, '\x00'));
			return r;
		});
		g_compress_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// conflux::http::security_headers_middleware test server
// ---------------------------------------------------------------------------

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
// ---------------------------------------------------------------------------
// conflux::http::cors_middleware test server
// ---------------------------------------------------------------------------

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
		router.use(conflux::http::cors_middleware()); // default: allowed_origins={"*"}, no credentials
		router.get("/api", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"ok":true})");
		});
		g_cors_wildcard_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// auth middleware test server
// ---------------------------------------------------------------------------

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
// ---------------------------------------------------------------------------
// conflux::http::rate_limit_middleware test server (2 req per 60s window)
// ---------------------------------------------------------------------------

std::uint16_t g_rate_port = 0;
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
std::uint16_t g_rate_burst_port = 0;
void ensure_rate_burst_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		// 1 base + 2 burst = 3 total capacity
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
// ---------------------------------------------------------------------------
// conflux::http::forwarded_middleware helpers
// ---------------------------------------------------------------------------

std::uint16_t g_fwd_port = 0;
std::uint16_t g_fwd_strict_empty_port = 0;
std::uint16_t g_fwd_lax_empty_port = 0;
void ensure_forwarded_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		// Trust only 127.0.0.1/32.
		router.use(conflux::http::forwarded_middleware({.trusted_proxies = {"127.0.0.1/32"}}));
		// Echo the remote_addr so tests can inspect it.
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
		// Default strict_mode=true, empty trusted_proxies → no peer is trusted.
		router.use(conflux::http::forwarded_middleware({}));
		router.get("/addr", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(req.remote_addr);
		});
		g_fwd_strict_empty_port = start_mw_server(mw_config(), std::move(router));
	});
}
void ensure_forwarded_lax_empty_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		// Legacy trust-all-on-empty behaviour.
		router.use(conflux::http::forwarded_middleware({.trusted_proxies = {}, .strict_mode = false}));
		router.get("/addr", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(req.remote_addr);
		});
		g_fwd_lax_empty_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// conflux::http::ip_filter_middleware helpers
// ---------------------------------------------------------------------------

std::uint16_t g_ipallow_port = 0;
std::uint16_t g_ipblock_port = 0;
void ensure_ipallow_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		// Allow only loopback.
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
		// Block loopback specifically.
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
		// Allowlist that does NOT include loopback → should block us.
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
		// Blocklist that does NOT include loopback → should pass us through.
		router.use(
			conflux::http::ip_filter_middleware({
				.mode = conflux::http::IpFilterMode::blocklist,
				.cidrs = {"10.0.0.0/8"},
			}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		g_ipblock_pass_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// conflux::http::cache_control_middleware helpers
// ---------------------------------------------------------------------------

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
// ---------------------------------------------------------------------------
// conflux::http::trailing_slash_middleware helpers
// ---------------------------------------------------------------------------

std::uint16_t g_ts_remove_port = 0;
std::uint16_t g_ts_add_port = 0;
void ensure_ts_remove_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::trailing_slash_middleware()); // default: remove
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
// Extract the value of a named header (case-sensitive) from a raw HTTP response.
// Returns empty std::string if not found.
std::string extract_header(
	std::string_view resp,
	std::string_view name) {
	// Search for "\r\nName: " (after the status line).
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
// Extract body (everything after the first blank line).
std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}
void check_problem_code(
	std::string_view resp,
	std::string_view code) {
	auto doc = conflux::json::parse_copy(extract_body(resp));
	REQUIRE(doc.has_value());

	auto code_node = doc->root().at_pointer("/code");
	REQUIRE(code_node.has_value());
	auto code_value = code_node->as_string();
	REQUIRE(code_value.has_value());
	CHECK(*code_value == code);

	auto diagnostic_node = doc->root().at_pointer("/diagnostic_code");
	REQUIRE(diagnostic_node.has_value());
	auto diagnostic_value = diagnostic_node->as_string();
	REQUIRE(diagnostic_value.has_value());
	CHECK(*diagnostic_value == code);
}
conflux::json::NodeRef require_json_pointer(
	conflux::json::Document const &doc,
	std::string_view pointer) {
	auto node = doc.root().at_pointer(pointer);
	REQUIRE(node.has_value());
	return *node;
}
void check_json_string_at(
	conflux::json::Document const &doc,
	std::string_view pointer,
	std::string_view expected) {
	auto node = require_json_pointer(doc, pointer);
	auto value = node.as_string();
	REQUIRE(value.has_value());
	CHECK(*value == expected);
}

} // namespace
// ---------------------------------------------------------------------------
// Basic connectivity
// ---------------------------------------------------------------------------

TEST_CASE(
	"GET / returns 200 with body") {
	auto resp = http_get("/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Hello from conflux") != std::string::npos);
}
TEST_CASE(
	"keep-alive header present on 200") {
	auto resp = http_get("/");
	REQUIRE(resp.find("Connection: keep-alive") != std::string::npos);
}
TEST_CASE(
	"Date header is present on HTTP/1.1 responses") {
	auto resp = http_get("/");
	auto date = extract_header(resp, "Date");
	REQUIRE(date.size() == std::string_view{"Wed, 29 Apr 2026 00:00:00 GMT"}.size());
	REQUIRE(date.ends_with(" GMT"));
}
TEST_CASE(
	"GET /api/ping returns JSON") {
	auto resp = http_get("/api/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("application/json") != std::string::npos);
	REQUIRE(resp.find(R"("status":"ok")") != std::string::npos);
}
TEST_CASE(
	"GET /api/task-ping returns root task payload") {
	auto resp = http_get("/api/task-ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find(R"("task":"ok")") != std::string::npos);
}
TEST_CASE(
	"GET /hello/{name} captures param") {
	auto resp = http_get("/hello/World");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Hello, World!") != std::string::npos);
}
TEST_CASE(
	"GET /hello/{name} captures different param") {
	auto resp = http_get("/hello/conflux");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Hello, conflux!") != std::string::npos);
}
TEST_CASE(
	"unregistered route returns 404") {
	auto resp = http_get("/does/not/exist");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}
TEST_CASE(
	"404 body contains the requested path") {
	auto resp = http_get("/nope");
	REQUIRE(resp.find("/nope") != std::string::npos);
}
// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------

TEST_CASE(
	"request header is accessible in handler") {
	auto resp = http_get_with_headers("/api/echo-header", "X-Test-Header: hello-world\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello-world");
}
TEST_CASE(
	"request header lookup is case-insensitive") {
	auto resp = http_get_with_headers("/api/echo-header", "x-test-header: case-test\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "case-test");
}
TEST_CASE(
	"missing request header returns 404") {
	auto resp = http_get("/api/echo-header");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}
// ---------------------------------------------------------------------------
// POST / body
// ---------------------------------------------------------------------------

TEST_CASE(
	"POST body is echoed back as plain text") {
	auto resp = http_post("/api/echo-body", "text/plain", "hello server");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello server");
}
TEST_CASE(
	"POST JSON body is echoed back with application/json content-type") {
	auto resp = http_post("/api/echo-json", "application/json", R"({"key":"value"})");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("application/json") != std::string::npos);
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == R"({"key":"value"})");
}
TEST_CASE(
	"POST with empty body is handled") {
	auto resp = http_post("/api/echo-body", "text/plain", "");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4).empty());
}
TEST_CASE(
	"POST with Expect: 100-continue receives interim response before body") {
	ensure_server();
	LocalTcpClient client{g_test_port};
	client.set_recv_timeout(std::chrono::seconds{5});

	std::string body = "hello server";
	auto req = std::format(
		"POST /api/echo-body HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: {}\r\n"
		"Expect: 100-continue\r\n"
		"Connection: close\r\n"
		"\r\n",
		body.size());
	(void)client.send(req);

	auto interim = client.read_headers();
	REQUIRE(interim.starts_with("HTTP/1.1 100 Continue"));

	(void)client.send(body);
	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	CHECK(resp.substr(hdr_end + 4) == body);
}
TEST_CASE(
	"POST with Expect: 100-continue supports pipelined follow-up request") {
	ensure_server();
	LocalTcpClient client{g_test_port};
	client.set_recv_timeout(std::chrono::seconds{5});

	std::string body = "first";
	auto req = std::format(
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\nContent-Length: {}\r\nExpect: "
		"100-continue\r\n\r\n",
		body.size());
	(void)client.send(req);
	auto interim = client.read_headers();
	REQUIRE(interim.starts_with("HTTP/1.1 100 Continue"));

	std::string rest = body;
	rest += "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	(void)client.send(rest);
	auto combined = client.read_until_close();
	auto first_pos = combined.find("HTTP/1.1 200 OK");
	REQUIRE(first_pos != std::string::npos);
	auto second_pos = combined.find("HTTP/1.1 200 OK", first_pos + 1);
	REQUIRE(second_pos != std::string::npos);
	auto first = combined.substr(first_pos, second_pos - first_pos);
	auto second = combined.substr(second_pos);
	REQUIRE(extract_body(first) == body);
	REQUIRE(extract_body(second) == R"({"status":"ok"})");
}
TEST_CASE(
	"POST with Expect: 100-continue works with chunked body") {
	ensure_server();
	LocalTcpClient client{g_test_port};
	client.set_recv_timeout(std::chrono::seconds{5});

	std::string_view const headers =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\nTransfer-Encoding: "
		"chunked\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
	(void)client.send(headers);
	auto interim = client.read_headers();
	REQUIRE(interim.starts_with("HTTP/1.1 100 Continue"));

	(void)client.send("5\r\nhello\r\n0\r\n\r\n");
	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "hello");
}
TEST_CASE(
	"POST with Expect: 100-continue times out if body never arrives") {
	Config cfg = Config::test();
	cfg.request_timeout_ms = 1500;
	conflux::http::Router router;
	router.post("/upload", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(req.body);
	});
	ScopedTestServer srv{cfg, std::move(router)};

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	timeval tv{.tv_sec = 5, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	std::string_view const req =
		"POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);
	auto interim = read_one_response(fd);
	REQUIRE(interim.starts_with("HTTP/1.1 100 Continue"));

	auto timeout_response = read_one_response(fd);
	::close(fd);
	REQUIRE(timeout_response.starts_with("HTTP/1.1 408 Request Timeout"));
	check_problem_code(timeout_response, "body_timeout");
}
TEST_CASE(
	"POST with unsupported Expect returns 417") {
	ensure_server();
	LocalTcpClient client{g_test_port};
	client.set_recv_timeout(std::chrono::seconds{5});
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Length: 5\r\n"
		"Expect: wait-for-magic\r\n"
		"Connection: close\r\n"
		"\r\n";
	(void)client.send(req);

	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 417 Expectation Failed"));
}
TEST_CASE(
	"POST to unknown route returns 404") {
	auto resp = http_post("/api/no-such-route", "text/plain", "body");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}
// ---------------------------------------------------------------------------
// Query std::string (GET form)
// ---------------------------------------------------------------------------

TEST_CASE(
	"query param is parsed and accessible") {
	auto resp = http_get("/api/echo-query?key=hello");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello");
}
TEST_CASE(
	"query param is percent-decoded") {
	auto resp = http_get("/api/echo-query?key=hello%20world");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello world");
}
TEST_CASE(
	"query param + is decoded as space") {
	auto resp = http_get("/api/echo-query?key=hello+world");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello world");
}
TEST_CASE(
	"query std::string does not bleed into path matching") {
	auto resp = http_get("/api/ping?ignored=1");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("application/json") != std::string::npos);
}
TEST_CASE(
	"missing query param returns 404") {
	auto resp = http_get("/api/echo-query");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}
// ---------------------------------------------------------------------------
// Unicode
// ---------------------------------------------------------------------------

TEST_CASE(
	"percent-encoded UTF-8 multibyte in query is decoded correctly") {
	// café: é = U+00E9 = %C3%A9
	auto resp = http_get("/api/echo-query?key=caf%C3%A9");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "café");
}
TEST_CASE(
	"percent-encoded emoji in query is decoded correctly") {
	// 😀 = U+1F600 = F0 9F 98 80
	auto resp = http_get("/api/echo-query?key=%F0%9F%98%80");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "😀");
}
TEST_CASE(
	"raw UTF-8 bytes in POST body pass through unchanged") {
	auto resp = http_post("/api/echo-body", "text/plain", "héllo");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "héllo");
}
TEST_CASE(
	"malformed percent sequence in query is passed through as-is") {
	// %GG is invalid hex — % emitted literally, GG follows as plain chars
	auto resp = http_get("/api/echo-query?key=%GGx");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "%GGx");
}
// ---------------------------------------------------------------------------
// conflux::http::Response headers
// ---------------------------------------------------------------------------

TEST_CASE(
	"custom response headers are sent to client") {
	auto resp = http_get("/api/with-header");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("X-Custom: hello\r\n") != std::string::npos);
	REQUIRE(resp.find("X-Another: world\r\n") != std::string::npos);
}
TEST_CASE(
	"response status_text with CRLF is sanitized") {
	Config cfg = mw_config();
	conflux::http::Router router;
	router.get("/bad-status", [](conflux::http::OwnedRequest const &) {
		conflux::http::Response r = conflux::http::Response::text("ok");
		r.status = 299;
		r.status_text = "Fine\r\nX-Injected: yes";
		return r;
	});
	ScopedTestServer srv{cfg, std::move(router)};
	auto resp = http_get_on(srv.port(), "/bad-status", "Connection: close\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 299 \r\n"));
	REQUIRE(resp.find("X-Injected: yes") == std::string::npos);
}
TEST_CASE(
	"302 redirect sets Location header and status") {
	auto resp = http_get("/api/redirect-302");
	REQUIRE(resp.starts_with("HTTP/1.1 302 Found"));
	REQUIRE(resp.find("Location: /api/ping\r\n") != std::string::npos);
}
TEST_CASE(
	"301 redirect sets Location header and status") {
	auto resp = http_get("/api/redirect-301");
	REQUIRE(resp.starts_with("HTTP/1.1 301 Moved Permanently"));
	REQUIRE(resp.find("Location: /api/ping\r\n") != std::string::npos);
}
TEST_CASE(
	"HTTPS redirect uses normalized absolute-form target") {
	Config cfg = mw_config();
	cfg.http_redirect_to_https = true;
	cfg.https_redirect_hosts = {"example.com"};
	conflux::http::Router router;
	router.get("/path", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};

	LocalTcpClient client{srv.port()};
	std::string_view const req =
		"GET http://ignored.test/path?q=1 HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
	(void)client.send(req);
	auto resp = client.read_until_close();
	REQUIRE(resp.starts_with("HTTP/1.1 308 Permanent Redirect"));
	REQUIRE(resp.find("Location: https://example.com/path?q=1\r\n") != std::string::npos);
	REQUIRE(resp.find("https://example.comhttp://") == std::string::npos);
}
// ---------------------------------------------------------------------------
// Keep-alive / persistent connections
// ---------------------------------------------------------------------------

TEST_CASE(
	"two sequential requests on one connection both succeed") {
	auto [r1, r2] = http_two_gets("/api/ping", "/");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r1.find("{\"status\":\"ok\"}") != std::string::npos);
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.find("Hello from conflux") != std::string::npos);
}
TEST_CASE(
	"Connection: close causes server to close after response") {
	REQUIRE(server_closed_after("/api/ping", "Connection: close\r\n"));
}
TEST_CASE(
	"HTTP/1.0 request causes server to close after response") {
	ensure_server();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_test_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	std::string_view const req = "GET /api/ping HTTP/1.0\r\nHost: localhost\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);
	read_one_response(fd);

	char probe{};
	auto n = ::recv(fd, &probe, 1, 0);
	::close(fd);
	REQUIRE(n == 0); // server closed
}
#include "http_e2e_middleware.cxx"
#include "http_e2e_observability.cxx"
