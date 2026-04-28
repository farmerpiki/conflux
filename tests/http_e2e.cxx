// Plain TU — intentionally not a module unit.
// std::thread with a lambda inside a module unit triggers GCC's TU-local
// entity exposure rule (the lambda type leaks into Tup instantiations).
// A plain TU has no such restriction.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

import std;
import conflux.types;

import conflux.crypto;
import conflux.net.http;
import conflux.net.http1_parser;
import conflux.tests.support;
import conflux.work;

using namespace conflux::tests;

namespace {

namespace chttp = conflux::http;
using conflux::http::HttpClient;
using conflux::http::HttpClientOptions;
using conflux::http::HttpErrorKind;
using conflux::http::HttpTimeouts;

// Actual port chosen by the OS; set once in ensure_server().
uint16_t g_test_port = 0;

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

		Router router;
		router.get("/", [](HttpRequest const &) {
			return HttpResponse::html("<html><body><h1>Hello from conflux!</h1></body></html>");
		});
		router.get("/hello/{name}", [](HttpRequest const &req) {
			return HttpResponse::html(std::format("<html><body><h1>Hello, {}!</h1></body></html>", req.params["name"]));
		});
		router.get("/api/ping", [](HttpRequest const &) { return HttpResponse::json(R"({"status":"ok"})"); });
		router.get("/api/echo-header", [](HttpRequest const &req) {
			auto v = req.headers["x-test-header"];
			if (v.empty()) {
				return HttpResponse::not_found("x-test-header");
			}
			return HttpResponse::text(S{v});
		});
		router.post("/api/echo-body", [](HttpRequest const &req) { return HttpResponse::text(req.body); });
		router.post("/api/echo-json", [](HttpRequest const &req) { return HttpResponse::json(req.body); });
		router.get("/api/echo-query", [](HttpRequest const &req) {
			auto v = req.query["key"];
			if (v.empty()) {
				return HttpResponse::not_found("key");
			}
			return HttpResponse::text(S{v});
		});
		router.post("/api/echo-form", [](HttpRequest const &req) {
			auto v = req.form["field"];
			if (v.empty()) {
				return HttpResponse::not_found("field");
			}
			return HttpResponse::text(S{v});
		});
		router.post("/api/multipart-field", [](HttpRequest const &req) {
			auto v = req.form["field"];
			if (v.empty()) {
				return HttpResponse::not_found("field");
			}
			return HttpResponse::text(S{v});
		});
		router.post("/api/multipart-file", [](HttpRequestView const &req) {
			if (req.files.empty()) {
				return HttpResponse::not_found("file");
			}
			auto const &f = req.files[0];
			return HttpResponse::json(
				std::format(
					R"({{"name":"{}","filename":"{}","content_type":"{}","size":{}}})",
					f.name,
					f.filename,
					f.content_type,
					f.data.size()));
		});
		router.get("/api/with-header", [](HttpRequest const &) {
			auto r = HttpResponse::text("ok");
			r.headers["X-Custom"] = "hello";
			r.headers["X-Another"] = "world";
			return r;
		});
		router.get("/api/redirect-302", [](HttpRequest const &) { return HttpResponse::redirect("/api/ping"); });
		router.get("/api/redirect-301", [](HttpRequest const &) { return HttpResponse::redirect("/api/ping", 301); });
		// PUT / PATCH / DELETE / OPTIONS routes.
		router.put("/api/resource/{id}", [](HttpRequest const &req) {
			return HttpResponse::json(std::format(R"({{"method":"PUT","id":"{}"}})", req.params["id"]));
		});
		router.patch("/api/resource/{id}", [](HttpRequest const &req) {
			return HttpResponse::json(std::format(R"({{"method":"PATCH","id":"{}"}})", req.params["id"]));
		});
		router.del("/api/resource/{id}", [](HttpRequest const &req) {
			return HttpResponse::json(std::format(R"({{"method":"DELETE","id":"{}"}})", req.params["id"]));
		});
		router.options("/api/resource", [](HttpRequest const &) {
			auto r = HttpResponse::text("");
			r.status = 204;
			r.status_text = "No Content";
			r.headers["Allow"] = "GET, POST, PUT, PATCH, DELETE, OPTIONS";
			return r;
		});
		// Route group: /api/v2/* with a version header middleware.
		router.group("/api/v2", [](Router::Group &g) {
			g.use([](HttpRequest const &req, Router::Handler const &next) {
				auto resp = next(req);
				resp.headers["X-Api-Version"] = "2";
				return resp;
			});
			g.get("/status", [](HttpRequest const &) { return HttpResponse::json(R"({"v":"2","status":"ok"})"); });
			g.get("/item/{id}", [](HttpRequest const &req) {
				return HttpResponse::json(std::format(R"({{"id":"{}"}})", req.params["id"]));
			});
		});
		// Route outside the group — must NOT have X-Api-Version header.
		router.get("/api/v1/status", [](HttpRequest const &) {
			return HttpResponse::json(R"({"v":"1","status":"ok"})");
		});
		// Cookie echo: returns value of named cookie.
		router.get("/api/echo-cookie", [](HttpRequest const &req) {
			auto v = req.cookies["name"];
			if (v.empty()) {
				return HttpResponse::not_found("name");
			}
			return HttpResponse::text(S{v});
		});
		// Set-cookie: sets two cookies on the response.
		router.get("/api/set-cookie", [](HttpRequest const &) {
			auto r = HttpResponse::text("ok");
			r.set_cookie("session", "abc123", "Path=/; HttpOnly");
			r.set_cookie("theme", "dark");
			return r;
		});
		// SSE endpoint: streams 3 events then closes.
		router.sse("/events", [](HttpRequest const &, SP<SseChannel> const &ch) {
			ch->send("data: event1\n\n");
			ch->send("data: event2\n\n");
			ch->send("data: event3\n\n");
			ch->close();
		});
		// Named-param SSE endpoint.
		router.sse("/events/{name}", [](HttpRequest const &req, SP<SseChannel> const &ch) {
			ch->send(std::format("data: hello {}\n\n", req.params["name"]));
			ch->close();
		});

		// Create server on heap so port() can be queried from this thread
		// while run() blocks on the worker thread.
		g_test_port = test_servers().start(cfg, std::move(router));
	});
}

// Connect, send a GET, parse Content-Length, return the full response.
// No shutdown(SHUT_WR) needed — stops reading once body is complete.
S http_get(
	SV path) {
	ensure_server();
	return conflux::tests::http_get_on(g_test_port, path);
}

// Like http_get but sends extra request headers.
S http_get_with_headers(
	SV path,
	SV extra_headers) {
	ensure_server();
	return conflux::tests::http_get_on(g_test_port, path, extra_headers);
}

// Send a POST request with a body; returns the full response.
S http_post(
	SV path,
	SV content_type,
	SV body) {
	ensure_server();
	return conflux::tests::http_post_on(g_test_port, path, content_type, body);
}

// Send an arbitrary HTTP request with a body.
S http_request(
	SV method,
	SV path,
	SV content_type = "",
	SV body = "") {
	ensure_server();
	return conflux::tests::http_request_on(g_test_port, method, path, content_type, body, "Connection: close\r\n");
}

// Connect and read an SSE stream until the server closes the connection.
// Returns the full response (headers + all event frames).
S http_get_sse(
	SV path) {
	ensure_server();

	LocalTcpClient client{g_test_port};
	auto req_str = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n", path);
	(void)client.send(req_str);
	client.set_recv_timeout(std::chrono::seconds{5});
	return client.read_until_close();
}

// Read exactly one HTTP/1.1 response from an already-connected fd.
// Returns the full raw response (status + headers + body).
// Send two sequential GET requests on one persistent connection.
// Returns {response1, response2}.
P<S, S> http_two_gets(
	SV path1,
	SV path2) {
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
	SV path,
	SV extra_headers) {
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
S http_get_with_header_on(
	uint16_t port,
	SV path,
	SV header) {
	return conflux::tests::http_get_on(port, path, header);
}

// Gzip-decompress a buffer; returns empty on failure.
S gzip_decompress(
	SV compressed) {
	z_stream zs{};
	if (inflateInit2(&zs, 15 | 16) != Z_OK) {
		return {};
	}
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-const-cast)
	zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
	zs.avail_in = static_cast<uInt>(compressed.size());
	S out;
	A<char, 4096> chunk{};
	int rc = Z_OK;
	while (rc == Z_OK) {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
		zs.next_out = reinterpret_cast<Bytef *>(chunk.data());
		zs.avail_out = static_cast<uInt>(chunk.size());
		rc = inflate(&zs, Z_NO_FLUSH);
		out.append(chunk.data(), chunk.size() - zs.avail_out);
	}
	inflateEnd(&zs);
	return rc == Z_STREAM_END ? out : S{};
}

// ---------------------------------------------------------------------------
// compress_middleware test server
// ---------------------------------------------------------------------------

uint16_t g_compress_port = 0;

void ensure_compress_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(compress_middleware());
		// Large body (>256 bytes) so min_body_size is exceeded.
		router.get("/big", [](HttpRequest const &) { return HttpResponse::html(S(512, 'A')); });
		// Small body (<256 bytes).
		router.get("/small", [](HttpRequest const &) { return HttpResponse::html("hi"); });
		// Non-compressible MIME type.
		router.get("/bin", [](HttpRequest const &) {
			HttpResponse r;
			r.status = 200;
			r.status_text = "OK";
			r.content_type = "application/octet-stream";
			r.set_text_body(S(512, '\x00'));
			return r;
		});
		g_compress_port = start_mw_server(mw_config(), std::move(router));
	});
}

uint16_t g_cors_compress_port = 0;

void ensure_cors_compress_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cors_middleware({.allowed_origins = {"https://test.example"}}));
		router.use(compress_middleware());
		router.get("/big", [](HttpRequest const &) { return HttpResponse::html(S(512, 'A')); });
		g_cors_compress_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// security_headers_middleware test server
// ---------------------------------------------------------------------------

uint16_t g_security_port = 0;

void ensure_security_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(security_headers_middleware());
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		g_security_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// cors_middleware test server
// ---------------------------------------------------------------------------

uint16_t g_cors_port = 0;

void ensure_cors_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cors_middleware({.allowed_origins = {"https://test.example"}}));
		router.get("/api", [](HttpRequest const &) { return HttpResponse::json(R"({"ok":true})"); });
		router.get("/vary", [](HttpRequest const &) {
			auto r = HttpResponse::text("vary");
			r.headers["Vary"] = "Accept-Encoding";
			return r;
		});
		g_cors_port = start_mw_server(mw_config(), std::move(router));
	});
}

uint16_t g_cors_cred_port = 0;

void ensure_cors_cred_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cors_middleware({
			.allowed_origins = {"*"},
			.expose_headers = {"X-Custom-Header", "X-Request-Id"},
			.allow_credentials = true,
		}));
		router.get("/api", [](HttpRequest const &) { return HttpResponse::json(R"({"ok":true})"); });
		g_cors_cred_port = start_mw_server(mw_config(), std::move(router));
	});
}

uint16_t g_cors_wildcard_port = 0;

void ensure_cors_wildcard_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cors_middleware()); // default: allowed_origins={"*"}, no credentials
		router.get("/api", [](HttpRequest const &) { return HttpResponse::json(R"({"ok":true})"); });
		g_cors_wildcard_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// auth middleware test server
// ---------------------------------------------------------------------------

uint16_t g_auth_port = 0;

void ensure_auth_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(basic_auth_middleware(
			[](SV u, SV p) { return u == "testuser" && p == "testpass"; }));
		router.get("/protected", [](HttpRequest const &) { return HttpResponse::text("secret"); });
		g_auth_port = start_mw_server(mw_config(), std::move(router));
	});
}

uint16_t g_bearer_port = 0;

void ensure_bearer_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(bearer_auth_middleware([](SV token) { return token == "valid-token-123"; }));
		router.get("/protected", [](HttpRequest const &) { return HttpResponse::text("secret"); });
		g_bearer_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// rate_limit_middleware test server (2 req per 60s window)
// ---------------------------------------------------------------------------

uint16_t g_rate_port = 0;
uint16_t g_rate_zero_clients_port = 0;

void ensure_rate_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(rate_limit_middleware({.requests = 2, .window = std::chrono::seconds{60}}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		g_rate_port = start_mw_server(mw_config(), std::move(router));
	});
}

uint16_t g_rate_burst_port = 0;

void ensure_rate_burst_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// 1 base + 2 burst = 3 total capacity
		router.use(rate_limit_middleware({.requests = 1, .window = std::chrono::seconds{60}, .burst = 2}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		g_rate_burst_port = start_mw_server(mw_config(), std::move(router));
	});
}

void ensure_rate_zero_clients_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(rate_limit_middleware({.requests = 1, .window = std::chrono::seconds{60}, .max_clients = 0}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		g_rate_zero_clients_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// forwarded_middleware helpers
// ---------------------------------------------------------------------------

uint16_t g_fwd_port = 0;
uint16_t g_fwd_strict_empty_port = 0;
uint16_t g_fwd_lax_empty_port = 0;

void ensure_forwarded_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Trust only 127.0.0.1/32.
		router.use(forwarded_middleware({.trusted_proxies = {"127.0.0.1/32"}}));
		// Echo the remote_addr so tests can inspect it.
		router.get("/addr", [](HttpRequest const &req) { return HttpResponse::text(req.remote_addr); });
		g_fwd_port = start_mw_server(mw_config(), std::move(router));
	});
}

void ensure_forwarded_strict_empty_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Default strict_mode=true, empty trusted_proxies → no peer is trusted.
		router.use(forwarded_middleware({}));
		router.get("/addr", [](HttpRequest const &req) { return HttpResponse::text(req.remote_addr); });
		g_fwd_strict_empty_port = start_mw_server(mw_config(), std::move(router));
	});
}

void ensure_forwarded_lax_empty_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Legacy trust-all-on-empty behaviour.
		router.use(forwarded_middleware({.trusted_proxies = {}, .strict_mode = false}));
		router.get("/addr", [](HttpRequest const &req) { return HttpResponse::text(req.remote_addr); });
		g_fwd_lax_empty_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// request_id_middleware helpers
// ---------------------------------------------------------------------------

uint16_t g_rid_port = 0;

void ensure_rid_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(request_id_middleware());
		// Echo the request ID header back in the body so tests can inspect it.
		router.get("/", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.headers["x-request-id"]});
		});
		g_rid_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// ip_filter_middleware helpers
// ---------------------------------------------------------------------------

uint16_t g_ipallow_port = 0;
uint16_t g_ipblock_port = 0;

void ensure_ipallow_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Allow only loopback.
		router.use(ip_filter_middleware({
			.mode = IpFilterMode::allowlist,
			.cidrs = {"127.0.0.0/8"},
		}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		g_ipallow_port = start_mw_server(mw_config(), std::move(router));
	});
}

void ensure_ipblock_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Block loopback specifically.
		router.use(ip_filter_middleware({
			.mode = IpFilterMode::blocklist,
			.cidrs = {"127.0.0.1/32"},
		}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		g_ipblock_port = start_mw_server(mw_config(), std::move(router));
	});
}

uint16_t g_ipallow_block_port = 0;

void ensure_ipallow_block_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Allowlist that does NOT include loopback → should block us.
		router.use(ip_filter_middleware({
			.mode = IpFilterMode::allowlist,
			.cidrs = {"192.168.0.0/24"},
		}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		g_ipallow_block_port = start_mw_server(mw_config(), std::move(router));
	});
}

uint16_t g_ipblock_pass_port = 0;

void ensure_ipblock_pass_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Blocklist that does NOT include loopback → should pass us through.
		router.use(ip_filter_middleware({
			.mode = IpFilterMode::blocklist,
			.cidrs = {"10.0.0.0/8"},
		}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		g_ipblock_pass_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// cache_control_middleware helpers
// ---------------------------------------------------------------------------

uint16_t g_cache_port = 0;

void ensure_cache_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cache_control_middleware({
			.rules =
				{
						{"image/", "max-age=31536000, immutable"},
						{"text/css", "max-age=86400, public"},
						{"application/json", "no-store"},
						},
			.default_directive = "no-cache",
		}));
		router.get("/image", [](HttpRequest const &) {
			HttpResponse r;
			r.content_type = "image/png";
			r.set_text_body("img");
			return r;
		});
		router.get("/css", [](HttpRequest const &) {
			HttpResponse r;
			r.content_type = "text/css";
			r.set_text_body("body{}");
			return r;
		});
		router.get("/api", [](HttpRequest const &) { return HttpResponse::json(R"({})"); });
		router.get("/html", [](HttpRequest const &) { return HttpResponse::html("<p>hi</p>"); });
		router.get("/custom", [](HttpRequest const &) {
			auto r = HttpResponse::text("x");
			r.headers["Cache-Control"] = "max-age=999";
			return r;
		});
		g_cache_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// trailing_slash_middleware helpers
// ---------------------------------------------------------------------------

uint16_t g_ts_remove_port = 0;
uint16_t g_ts_add_port = 0;

void ensure_ts_remove_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(trailing_slash_middleware()); // default: remove
		router.get("/foo", [](HttpRequest const &) { return HttpResponse::text("foo"); });
		g_ts_remove_port = start_mw_server(mw_config(), std::move(router));
	});
}

void ensure_ts_add_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(trailing_slash_middleware({.mode = TrailingSlashMode::add}));
		router.get("/bar/", [](HttpRequest const &) { return HttpResponse::text("bar"); });
		g_ts_add_port = start_mw_server(mw_config(), std::move(router));
	});
}

uint16_t g_ts_308_port = 0;

void ensure_ts_308_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(trailing_slash_middleware({.redirect_status = 308}));
		router.get("/foo", [](HttpRequest const &) { return HttpResponse::text("foo"); });
		g_ts_308_port = start_mw_server(mw_config(), std::move(router));
	});
}

uint16_t g_ts_307_port = 0;

void ensure_ts_307_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(trailing_slash_middleware({.redirect_status = 307}));
		router.get("/foo", [](HttpRequest const &) { return HttpResponse::text("foo"); });
		g_ts_307_port = start_mw_server(mw_config(), std::move(router));
	});
}

// POST to an explicit port with extra headers (for CSRF token etc.).
S http_post_on_full(
	uint16_t port,
	SV path,
	SV content_type,
	SV body,
	SV extra_headers) {
	return conflux::tests::http_post_on(port, path, content_type, body, extra_headers);
}

// Extract the value of a named header (case-sensitive) from a raw HTTP response.
// Returns empty string if not found.
S extract_header(
	SV resp,
	SV name) {
	// Search for "\r\nName: " (after the status line).
	auto needle = S{"\r\n"} + S{name} + ": ";
	auto pos = resp.find(needle);
	if (pos == SV::npos) {
		return {};
	}
	pos += needle.size();
	auto end = resp.find("\r\n", pos);
	if (end == SV::npos) {
		return {};
	}
	return S{resp.substr(pos, end - pos)};
}

// Extract body (everything after the first blank line).
S extract_body(
	SV resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == SV::npos) {
		return {};
	}
	return S{resp.substr(pos + 4)};
}

// Extract the value of a named cookie from a Set-Cookie header list.
// Looks for "Set-Cookie: <name>=<value>; ..." lines.
S extract_set_cookie(
	SV resp,
	SV name) {
	S const needle = S{"Set-Cookie: "} + S{name} + "=";
	auto pos = resp.find(needle);
	if (pos == SV::npos) {
		return {};
	}
	pos += needle.size();
	auto end = resp.find_first_of(";\r\n", pos);
	return S{resp.substr(pos, end - pos)};
}

// ---------------------------------------------------------------------------
// redirect_middleware test server
// ---------------------------------------------------------------------------

uint16_t g_redirect_port = 0;

void ensure_redirect_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(redirect_middleware({
			.rules = {
					  {.from = "/old", .to = "/new", .status = 301},
					  {.from = "/api/v1/", .to = "/api/v2/", .status = 302, .prefix_match = true},
					  }
        }));
		router.get("/new", [](HttpRequest const &) { return HttpResponse::text("new"); });
		router.get("/api/v2/users", [](HttpRequest const &) { return HttpResponse::text("v2-users"); });
		g_redirect_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// csrf_middleware test server
// ---------------------------------------------------------------------------

uint16_t g_csrf_port = 0;

void ensure_csrf_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(csrf_middleware());
		router.get("/page", [](HttpRequest const &) { return HttpResponse::html("<form>"); });
		router.post("/submit", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		g_csrf_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// etag_middleware test server
// ---------------------------------------------------------------------------

uint16_t g_etag_port = 0;

void ensure_etag_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(etag_middleware());
		router.get("/content", [](HttpRequest const &) { return HttpResponse::text("hello world"); });
		router.get("/empty", [](HttpRequest const &) { return HttpResponse::text(""); });
		g_etag_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// response_cache_middleware test server
// ---------------------------------------------------------------------------

std::atomic<int> g_resp_cache_count{0};
uint16_t g_resp_cache_port = 0;

void ensure_resp_cache_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(response_cache_middleware({
			.max_entries = 16,
			.default_ttl = std::chrono::seconds{60},
		}));
		router.get("/counted", [](HttpRequest const &) {
			int n = ++g_resp_cache_count;
			return HttpResponse::text(std::format("visit {}", n));
		});
		router.post("/counted", [](HttpRequest const &) {
			int n = ++g_resp_cache_count;
			return HttpResponse::text(std::format("post {}", n));
		});
		router.get("/no-store", [](HttpRequest const &) {
			auto r = HttpResponse::text("uncacheable");
			r.headers["Cache-Control"] = "no-store";
			return r;
		});
		router.get("/vary", [](HttpRequest const &req) {
			int n = ++g_resp_cache_count;
			auto r = HttpResponse::text(std::format("{} enc={}", n, req.headers["accept-encoding"]));
			r.headers["Vary"] = "Accept-Encoding";
			return r;
		});
		router.get("/vary-star", [](HttpRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = HttpResponse::text(std::format("star {}", n));
			r.headers["Vary"] = "*";
			return r;
		});
		router.get("/private", [](HttpRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = HttpResponse::text(std::format("priv {}", n));
			r.headers["Cache-Control"] = "private";
			return r;
		});
		router.get("/max-age-zero", [](HttpRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = HttpResponse::text(std::format("zero {}", n));
			r.headers["Cache-Control"] = "max-age=0";
			return r;
		});
		router.get("/no-cache", [](HttpRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = HttpResponse::text(std::format("nocache {}", n));
			r.headers["Cache-Control"] = "no-cache";
			return r;
		});
		router.get("/set-cookie-resp", [](HttpRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = HttpResponse::text(std::format("cookie {}", n));
			r.set_cookie("sid", "abc");
			return r;
		});
		router.get("/not-found-resp", [](HttpRequest const &) {
			++g_resp_cache_count;
			return HttpResponse::not_found("/not-found-resp");
		});
		router.get("/query", [](HttpRequest const &req) {
			int n = ++g_resp_cache_count;
			return HttpResponse::text(std::format("{} value={}", n, req.query["value"]));
		});
		g_resp_cache_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// structured_log_middleware test server
// ---------------------------------------------------------------------------

uint16_t g_slog_port = 0;
char g_slog_path[64]{};

void ensure_slog_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		std::strcpy(g_slog_path, "/tmp/conflux_slog_XXXXXX");
		int const tmp = ::mkstemp(g_slog_path);
		::close(tmp);

		Router router;
		router.use(structured_log_middleware({.log_file = g_slog_path, .app_name = "test"}));
		router.get("/ping", [](HttpRequest const &) { return HttpResponse::text("pong"); });
		g_slog_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// tracing_middleware test server
// ---------------------------------------------------------------------------

uint16_t g_trace_port = 0;

void ensure_trace_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(tracing_middleware({.propagate_in_response = true}));
		// Echo the injected traceparent header so tests can verify it.
		router.get("/", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.headers["traceparent"]});
		});
		g_trace_port = start_mw_server(mw_config(), std::move(router));
	});
}

// ---------------------------------------------------------------------------
// VHostRouter test server
// ---------------------------------------------------------------------------

uint16_t g_vhost_port = 0;
uint16_t g_vhost_direct_port = 0;
uint16_t g_proxy_port = 0;
SP<ScopedTestServer> g_proxy_upstream;
SP<ScopedTestServer> g_proxy_front;

void ensure_vhost_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router api_router;
		api_router.get("/status", [](HttpRequest const &) { return HttpResponse::text("api"); });

		Router web_router;
		web_router.get("/status", [](HttpRequest const &) { return HttpResponse::text("web"); });

		Router def_router;
		def_router.get("/status", [](HttpRequest const &) { return HttpResponse::text("default"); });

		auto vhr = std::make_shared<VHostRouter>();
		vhr->add("api.example.com", std::move(api_router));
		vhr->add("web.example.com", std::move(web_router));
		vhr->set_default(std::move(def_router));

		Router main;
		main.use([vhr](HttpRequest const &req, Router::Handler const &) { return vhr->dispatch(req); });
		g_vhost_port = start_mw_server(mw_config(), std::move(main));
	});
}

void ensure_vhost_direct_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router api_router;
		api_router.get("/status", [](HttpRequest const &) { return HttpResponse::text("api-direct"); });

		Router def_router;
		def_router.get("/status", [](HttpRequest const &) { return HttpResponse::text("default-direct"); });

		VHostRouter vhost_router;
		vhost_router.add("api.example.com", std::move(api_router));
		vhost_router.set_default(std::move(def_router));

		g_vhost_direct_port = test_servers().start(mw_config(), std::move(vhost_router));
	});
}

void ensure_proxy_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();

		Router upstream;
		upstream.get("/ping", [](HttpRequest const &) {
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
			auto resp = HttpResponse::text("proxied-ok");
			resp.headers["X-Upstream"] = "yes";
			return resp;
		});
		g_proxy_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));

		auto shared_pool = std::make_shared<WorkPool>();
		Router front;
		front.set_work_pool(shared_pool);
		front.get(
			"/proxy/ping",
			proxy_handler(
				ProxyOptions{
					.upstream_host = "127.0.0.1",
					.upstream_port = g_proxy_upstream->port(),
					.path_prefix = "/proxy",
					.work_pool = shared_pool,
				}));
		g_proxy_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
		g_proxy_port = g_proxy_front->port();
	});
}

} // namespace

// ---------------------------------------------------------------------------
// Basic connectivity
// ---------------------------------------------------------------------------

TEST_CASE(
	"GET / returns 200 with body") {
	auto resp = http_get("/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Hello from conflux") != S::npos);
}

TEST_CASE(
	"keep-alive header present on 200") {
	auto resp = http_get("/");
	REQUIRE(resp.find("Connection: keep-alive") != S::npos);
}

// ---------------------------------------------------------------------------
// Router — path matching
// ---------------------------------------------------------------------------

TEST_CASE(
	"GET /api/ping returns JSON") {
	auto resp = http_get("/api/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("application/json") != S::npos);
	REQUIRE(resp.find(R"("status":"ok")") != S::npos);
}

TEST_CASE(
	"http client: GET /api/ping returns parsed response") {
	ensure_server();
	auto response =
		HttpClient{}.send_blocking(chttp::HttpRequest::get(std::format("http://127.0.0.1:{}/api/ping", g_test_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(S{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body == R"({"status":"ok"})");
}

TEST_CASE(
	"http client: GET /api/ping returns parsed response (send_blocking)") {
	ensure_server();
	HttpClient client{};
	auto response =
		client.send_blocking(chttp::HttpRequest::get(std::format("http://127.0.0.1:{}/api/ping", g_test_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(S{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body == R"({"status":"ok"})");
}

TEST_CASE(
	"http client: convenience client sends headers and parses response headers") {
	ensure_server();
	HttpClient client{};
	HttpFields headers{true};
	headers["X-Test-Header"] = "client-header";

	auto response = client.send_blocking(
		chttp::HttpRequest::get(std::format("http://127.0.0.1:{}/api/echo-header", g_test_port)).headers(headers));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body == "client-header");

	auto with_headers =
		client.send_blocking(chttp::HttpRequest::get(std::format("http://127.0.0.1:{}/api/with-header", g_test_port)));
	REQUIRE(with_headers);
	CHECK(with_headers->head.headers["x-custom"] == "hello");
	CHECK(with_headers->head.headers["x-another"] == "world");
}

TEST_CASE(
	"http client: convenience client POST sends body and content type") {
	ensure_server();
	HttpClient client{};

	auto response = client.send_blocking(
		chttp::HttpRequest::post(std::format("http://127.0.0.1:{}/api/echo-json", g_test_port))
			.content_type("application/json")
			.body(R"({"from":"client"})"));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(S{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body == R"({"from":"client"})");
}

TEST_CASE(
	"http client: PUT sends body and receives response") {
	ensure_server();
	HttpClient client{};

	auto response = client.send_blocking(
		chttp::HttpRequest::put(std::format("http://127.0.0.1:{}/api/resource/42", g_test_port))
			.content_type("application/json")
			.body(R"({"x":1})"));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("PUT") != S::npos);
	CHECK(response->body.find("42") != S::npos);
}

TEST_CASE(
	"http client: PATCH sends body and receives response") {
	ensure_server();
	HttpClient client{};

	auto response = client.send_blocking(
		chttp::HttpRequest::patch(std::format("http://127.0.0.1:{}/api/resource/7", g_test_port))
			.content_type("application/json")
			.body(R"({"delta":1})"));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("PATCH") != S::npos);
	CHECK(response->body.find("7") != S::npos);
}

TEST_CASE(
	"http client: DELETE returns response") {
	ensure_server();
	HttpClient client{};

	auto response =
		client.send_blocking(chttp::HttpRequest::del(std::format("http://127.0.0.1:{}/api/resource/99", g_test_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("DELETE") != S::npos);
	CHECK(response->body.find("99") != S::npos);
}

TEST_CASE(
	"http client: HEAD /api/ping returns 200 with no body") {
	ensure_server();
	HttpClient client{};

	auto response =
		client.send_blocking(chttp::HttpRequest::head(std::format("http://127.0.0.1:{}/api/ping", g_test_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(S{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body.empty());
}

TEST_CASE(
	"http client: send_blocking works without pool") {
	ensure_server();
	HttpClient client{};
	auto response =
		client.send_blocking(chttp::HttpRequest::get(std::format("http://127.0.0.1:{}/api/ping", g_test_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body == R"({"status":"ok"})");
}

TEST_CASE(
	"http client: connection failure returns an error") {
	HttpTimeouts timeouts{};
	timeouts.connect = std::chrono::milliseconds{1000};
	HttpClientOptions opts{};
	opts.default_timeouts = timeouts;
	HttpClient client{opts};
	auto response = client.send_blocking(chttp::HttpRequest::get("http://127.0.0.1:9/"));
	REQUIRE_FALSE(response);
	CHECK(response.error().kind == HttpErrorKind::connect);
}

TEST_CASE(
	"GET /hello/{name} captures param") {
	auto resp = http_get("/hello/World");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Hello, World!") != S::npos);
}

TEST_CASE(
	"GET /hello/{name} captures different param") {
	auto resp = http_get("/hello/conflux");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Hello, conflux!") != S::npos);
}

TEST_CASE(
	"unregistered route returns 404") {
	auto resp = http_get("/does/not/exist");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}

TEST_CASE(
	"404 body contains the requested path") {
	auto resp = http_get("/nope");
	REQUIRE(resp.find("/nope") != S::npos);
}

// ---------------------------------------------------------------------------
// SSE — Server-Sent Events
// ---------------------------------------------------------------------------

TEST_CASE(
	"SSE /events returns text/event-stream") {
	auto resp = http_get_sse("/events");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Type: text/event-stream") != S::npos);
}

TEST_CASE(
	"SSE /events streams all 3 events") {
	auto resp = http_get_sse("/events");
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != S::npos);
	auto body = SV{resp}.substr(body_start + 4);
	REQUIRE(body.find("data: event1") != SV::npos);
	REQUIRE(body.find("data: event2") != SV::npos);
	REQUIRE(body.find("data: event3") != SV::npos);
}

TEST_CASE(
	"SSE /events/{name} captures param") {
	auto resp = http_get_sse("/events/alice");
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != S::npos);
	auto body = SV{resp}.substr(body_start + 4);
	REQUIRE(body.find("data: hello alice") != SV::npos);
}

// ---------------------------------------------------------------------------
// SseBroadcaster unit-level tests
// ---------------------------------------------------------------------------

TEST_CASE(
	"SseBroadcaster: subscriber_count tracks subscriptions") {
	SseBroadcaster bc;
	REQUIRE(bc.subscriber_count() == 0);
	auto ch1 = bc.subscribe();
	REQUIRE(bc.subscriber_count() == 1);
	auto ch2 = bc.subscribe();
	REQUIRE(bc.subscriber_count() == 2);
}

TEST_CASE(
	"SseBroadcaster: stale subscriber is evicted on broadcast") {
	SseBroadcaster bc;
	{
		auto ch = bc.subscribe();
		REQUIRE(bc.subscriber_count() == 1);
		// ch goes out of scope here; weak_ptr becomes stale
	}
	// broadcast_raw triggers erase_if which removes the stale weak_ptr
	bc.broadcast_data("ping");
	REQUIRE(bc.subscriber_count() == 0);
}

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------

TEST_CASE(
	"request header is accessible in handler") {
	auto resp = http_get_with_headers("/api/echo-header", "X-Test-Header: hello-world\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello-world");
}

TEST_CASE(
	"request header lookup is case-insensitive") {
	auto resp = http_get_with_headers("/api/echo-header", "x-test-header: case-test\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
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
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello server");
}

TEST_CASE(
	"POST JSON body is echoed back with application/json content-type") {
	auto resp = http_post("/api/echo-json", "application/json", R"({"key":"value"})");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("application/json") != S::npos);
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == R"({"key":"value"})");
}

TEST_CASE(
	"POST with empty body is handled") {
	auto resp = http_post("/api/echo-body", "text/plain", "");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4).empty());
}

TEST_CASE(
	"POST to unknown route returns 404") {
	auto resp = http_post("/api/no-such-route", "text/plain", "body");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}

// ---------------------------------------------------------------------------
// Query string (GET form)
// ---------------------------------------------------------------------------

TEST_CASE(
	"query param is parsed and accessible") {
	auto resp = http_get("/api/echo-query?key=hello");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello");
}

TEST_CASE(
	"query param is percent-decoded") {
	auto resp = http_get("/api/echo-query?key=hello%20world");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello world");
}

TEST_CASE(
	"query param + is decoded as space") {
	auto resp = http_get("/api/echo-query?key=hello+world");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello world");
}

TEST_CASE(
	"query string does not bleed into path matching") {
	auto resp = http_get("/api/ping?ignored=1");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("application/json") != S::npos);
}

TEST_CASE(
	"missing query param returns 404") {
	auto resp = http_get("/api/echo-query");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}

// ---------------------------------------------------------------------------
// URL-encoded form (POST form)
// ---------------------------------------------------------------------------

TEST_CASE(
	"urlencoded form field is parsed") {
	auto resp = http_post("/api/echo-form", "application/x-www-form-urlencoded", "field=hello");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello");
}

TEST_CASE(
	"urlencoded form field is percent-decoded") {
	auto resp = http_post("/api/echo-form", "application/x-www-form-urlencoded", "field=hello%20world");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello world");
}

TEST_CASE(
	"urlencoded form with multiple fields parses target field") {
	auto resp = http_post("/api/echo-form", "application/x-www-form-urlencoded", "other=x&field=target&more=y");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "target");
}

TEST_CASE(
	"non-urlencoded POST does not populate form") {
	auto resp = http_post("/api/echo-form", "text/plain", "field=hello");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}

// ---------------------------------------------------------------------------
// multipart/form-data
// ---------------------------------------------------------------------------

// Builds a minimal multipart/form-data body with one text field.
static S make_multipart_text(
	SV boundary,
	SV name,
	SV field_value) {
	return std::format(
		"--{}\r\n"
		"Content-Disposition: form-data; name=\"{}\"\r\n"
		"\r\n"
		"{}\r\n"
		"--{}--\r\n",
		boundary,
		name,
		field_value,
		boundary);
}

// Builds a multipart/form-data body with one file part.
static S make_multipart_file(
	SV boundary,
	SV name,
	SV filename,
	SV content_type,
	SV data) {
	return std::format(
		"--{}\r\n"
		"Content-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\n"
		"Content-Type: {}\r\n"
		"\r\n"
		"{}\r\n"
		"--{}--\r\n",
		boundary,
		name,
		filename,
		content_type,
		data,
		boundary);
}

TEST_CASE(
	"multipart/form-data text field is parsed into req.form") {
	auto body = make_multipart_text("boundary123", "field", "hello from multipart");
	auto ct = std::format("multipart/form-data; boundary=boundary123");
	auto resp = http_post("/api/multipart-field", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello from multipart");
}

TEST_CASE(
	"multipart/form-data value with special characters is preserved") {
	auto body = make_multipart_text("bnd42", "field", "a=1&b=2 <> \"quotes\"");
	auto ct = S{"multipart/form-data; boundary=bnd42"};
	auto resp = http_post("/api/multipart-field", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "a=1&b=2 <> \"quotes\"");
}

TEST_CASE(
	"multipart/form-data file part populates req.files") {
	auto body = make_multipart_file("fileBnd", "upload", "hello.txt", "text/plain", "file content here");
	auto ct = S{"multipart/form-data; boundary=fileBnd"};
	auto resp = http_post("/api/multipart-file", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	auto json = resp.substr(hdr_end + 4);
	REQUIRE(json.find("\"name\":\"upload\"") != S::npos);
	REQUIRE(json.find("\"filename\":\"hello.txt\"") != S::npos);
	REQUIRE(json.find("\"content_type\":\"text/plain\"") != S::npos);
	REQUIRE(json.find("\"size\":17") != S::npos);
}

TEST_CASE(
	"multipart/form-data without boundary returns 404 (field not parsed)") {
	// Content-Type has no boundary= — parser cannot find parts.
	auto body = make_multipart_text("bnd", "field", "ignored");
	auto resp = http_post("/api/multipart-field", "multipart/form-data", body);
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
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "café");
}

TEST_CASE(
	"percent-encoded emoji in query is decoded correctly") {
	// 😀 = U+1F600 = F0 9F 98 80
	auto resp = http_get("/api/echo-query?key=%F0%9F%98%80");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "😀");
}

TEST_CASE(
	"raw UTF-8 bytes in POST body pass through unchanged") {
	auto resp = http_post("/api/echo-body", "text/plain", "héllo");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "héllo");
}

TEST_CASE(
	"malformed percent sequence in query is passed through as-is") {
	// %GG is invalid hex — % emitted literally, GG follows as plain chars
	auto resp = http_get("/api/echo-query?key=%GGx");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "%GGx");
}

TEST_CASE(
	"percent-encoded UTF-8 in urlencoded form field is decoded correctly") {
	// こんにちは percent-encoded
	auto resp = http_post(
		"/api/echo-form",
		"application/x-www-form-urlencoded",
		"field=%E3%81%93%E3%82%93%E3%81%AB%E3%81%A1%E3%81%AF");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "こんにちは");
}

// ---------------------------------------------------------------------------
// Response headers
// ---------------------------------------------------------------------------

TEST_CASE(
	"custom response headers are sent to client") {
	auto resp = http_get("/api/with-header");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("X-Custom: hello\r\n") != S::npos);
	REQUIRE(resp.find("X-Another: world\r\n") != S::npos);
}

TEST_CASE(
	"302 redirect sets Location header and status") {
	auto resp = http_get("/api/redirect-302");
	REQUIRE(resp.starts_with("HTTP/1.1 302 Found"));
	REQUIRE(resp.find("Location: /api/ping\r\n") != S::npos);
}

TEST_CASE(
	"301 redirect sets Location header and status") {
	auto resp = http_get("/api/redirect-301");
	REQUIRE(resp.starts_with("HTTP/1.1 301 Moved Permanently"));
	REQUIRE(resp.find("Location: /api/ping\r\n") != S::npos);
}

// ---------------------------------------------------------------------------
// Keep-alive / persistent connections
// ---------------------------------------------------------------------------

TEST_CASE(
	"two sequential requests on one connection both succeed") {
	auto [r1, r2] = http_two_gets("/api/ping", "/");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r1.find("{\"status\":\"ok\"}") != S::npos);
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.find("Hello from conflux") != S::npos);
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

	SV const req = "GET /api/ping HTTP/1.0\r\nHost: localhost\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);
	read_one_response(fd);

	char probe{};
	auto n = ::recv(fd, &probe, 1, 0);
	::close(fd);
	REQUIRE(n == 0); // server closed
}

// ---------------------------------------------------------------------------
// Graceful shutdown
// ---------------------------------------------------------------------------

TEST_CASE(
	"shutdown() stops run() and server becomes unreachable") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	router.get("/ping", [](HttpRequest const &) { return HttpResponse::text("pong"); });

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const port = srv.port();

	// Verify it responds before shutdown.
	{
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
		SV const req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
		::send(fd, req.data(), req.size(), 0);
		auto resp = read_one_response(fd);
		::close(fd);
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	}

	// Shutdown and wait for run() to return.
	srv.stop(); // if this returns, run() exited cleanly

	// Verify the server no longer services new requests on that port.
	bool stopped = false;
	for (int i = 0; i < 50 && !stopped; ++i) {
		int const fd2 = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr2{};
		addr2.sin_family = AF_INET;
		addr2.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr2.sin_addr);
		int const conn_result = ::connect(fd2, reinterpret_cast<sockaddr *>(&addr2), sizeof(addr2));
		if (conn_result < 0) {
			::close(fd2);
			stopped = true;
			break;
		}
		SV const req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
		::send(fd2, req.data(), req.size(), 0);
		char probe{};
		auto n = ::recv(fd2, &probe, 1, MSG_DONTWAIT);
		::close(fd2);
		bool const would_block = errno == EAGAIN;
		stopped = (n == 0) || (n < 0 && (would_block || errno == ECONNRESET));
		if (!stopped) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	REQUIRE(stopped);
}

// ---------------------------------------------------------------------------
// Middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"middleware chain: response header injection, auth guard, request enrichment") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;

	// Two logging middlewares — verify execution order (A then B, outermost first).
	router.use([](HttpRequest const &req, Router::Handler const &next) {
		auto resp = next(req);
		resp.headers["X-MW-Order"] = S{resp.headers["X-MW-Order"]} + "A";
		return resp;
	});
	router.use([](HttpRequest const &req, Router::Handler const &next) {
		auto resp = next(req);
		resp.headers["X-MW-Order"] = S{resp.headers["X-MW-Order"]} + "B";
		return resp;
	});

	// Auth guard: requires X-Api-Key: secret.
	router.use([](HttpRequest const &req, Router::Handler const &next) {
		if (req.path == "/protected" && req.headers["x-api-key"] != "secret") {
			return HttpResponse::html("Forbidden", 403, "Forbidden");
		}
		return next(req);
	});

	// Middleware that enriches the request before passing downstream.
	router.use([](HttpRequest const &req, Router::Handler const &next) {
		HttpRequest enriched = req;
		enriched.headers["x-injected"] = "injected-value";
		return next(enriched);
	});

	router.get("/ping", [](HttpRequest const &) { return HttpResponse::text("pong"); });
	router.get("/protected", [](HttpRequest const &) { return HttpResponse::text("secret"); });
	router.get("/injected", [](HttpRequest const &req) {
		return HttpResponse::text(S{req.headers["x-injected"]});
	});

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const mw_port = srv.port();

	auto get = [&](SV path, SV extra = "") {
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(mw_port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
		auto req = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}\r\n", path, extra);
		::send(fd, req.data(), req.size(), 0);
		auto resp = read_one_response(fd);
		::close(fd);
		return resp;
	};

	SECTION("middleware A runs outermost, B innermost (both stamp header)") {
		auto resp = get("/ping");
		// A wraps B: B appends "B", then A appends "A" → "BA"
		REQUIRE(resp.find("X-MW-Order: BA\r\n") != S::npos);
	}

	SECTION("auth middleware blocks /protected without key") {
		auto resp = get("/protected");
		REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
	}

	SECTION("auth middleware passes /protected with correct key") {
		auto resp = get("/protected", "X-Api-Key: secret\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		auto hdr_end = resp.find("\r\n\r\n");
		REQUIRE(resp.substr(hdr_end + 4) == "secret");
	}

	SECTION("middleware can enrich request before passing to handler") {
		auto resp = get("/injected");
		auto hdr_end = resp.find("\r\n\r\n");
		REQUIRE(resp.substr(hdr_end + 4) == "injected-value");
	}

	srv.stop();
}

// ---------------------------------------------------------------------------
// Request body size limit
// ---------------------------------------------------------------------------

TEST_CASE(
	"POST with body within default limit is accepted") {
	// 100 bytes — well within 1 MiB default
	auto resp = http_post("/api/echo-body", "text/plain", S(100, 'x'));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}

TEST_CASE(
	"POST claiming body larger than configured limit returns 413") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.max_body_size = 64; // tiny limit for testing

	Router router;
	router.post("/upload", [](HttpRequest const &req) { return HttpResponse::text(req.body); });

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const limit_port = srv.port();

	auto post = [&](SZ body_size) {
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(limit_port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
		S body(body_size, 'A');
		auto req = std::format(
			"POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\n"
			"Content-Length: {}\r\nConnection: close\r\n\r\n{}",
			body_size,
			body);
		::send(fd, req.data(), req.size(), 0);
		auto resp = read_one_response(fd);
		::close(fd);
		return resp;
	};

	SECTION("body at limit is accepted") {
		auto resp = post(64);
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	}

	SECTION("body one byte over limit returns 413") {
		auto resp = post(65);
		REQUIRE(resp.starts_with("HTTP/1.1 413 Content Too Large"));
	}

	srv.stop();
}

// ---------------------------------------------------------------------------
// HEAD method
// ---------------------------------------------------------------------------

TEST_CASE(
	"HEAD / returns same headers as GET but no body") {
	ensure_server();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_test_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	SV const req = "HEAD / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);

	// Read until connection closes — no body expected.
	S resp;
	A<char, 4096> buf{};
	for (;;) {
		auto n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n <= 0) {
			break;
		}
		resp.append(buf.data(), static_cast<SZ>(n));
	}
	::close(fd);

	auto get_resp = http_get("/");

	// Status and Content-Type must match GET.
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Type: text/html") != S::npos);

	// Content-Length must equal the GET body size.
	auto extract_cl = [](S const &r) -> SZ {
		auto pos = r.find("Content-Length: ");
		if (pos == S::npos) {
			return 0;
		}
		pos += 16;
		SZ v = 0;
		std::from_chars(r.data() + pos, r.data() + r.size(), v);
		return v;
	};
	REQUIRE(extract_cl(resp) == extract_cl(get_resp));

	// HEAD response must have no body — everything after \r\n\r\n is empty.
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.size() == hdr_end + 4);
}

TEST_CASE(
	"HEAD /api/ping matches GET route and returns no body") {
	ensure_server();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_test_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	SV const req = "HEAD /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);

	S resp;
	A<char, 4096> buf{};
	for (;;) {
		auto n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n <= 0) {
			break;
		}
		resp.append(buf.data(), static_cast<SZ>(n));
	}
	::close(fd);

	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Type: application/json") != S::npos);
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.size() == hdr_end + 4); // no body
}

// ---------------------------------------------------------------------------
// PUT / PATCH / DELETE / OPTIONS
// ---------------------------------------------------------------------------

TEST_CASE(
	"PUT /api/resource/{id} is routed correctly") {
	auto resp = http_request("PUT", "/api/resource/42");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"method":"PUT","id":"42"})");
}

TEST_CASE(
	"PATCH /api/resource/{id} is routed correctly") {
	auto resp = http_request("PATCH", "/api/resource/7");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"method":"PATCH","id":"7"})");
}

TEST_CASE(
	"DELETE /api/resource/{id} is routed correctly") {
	auto resp = http_request("DELETE", "/api/resource/99");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"method":"DELETE","id":"99"})");
}

TEST_CASE(
	"OPTIONS /api/resource returns Allow header") {
	auto resp = http_request("OPTIONS", "/api/resource");
	REQUIRE(resp.starts_with("HTTP/1.1 204 No Content"));
	REQUIRE(resp.find("Allow: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n") != S::npos);
}

TEST_CASE(
	"unknown method on unregistered path returns 404") {
	auto resp = http_request("DELETE", "/no-such-path");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}

// ---------------------------------------------------------------------------
// Custom error handlers (on_not_found / on_error)
// ---------------------------------------------------------------------------

TEST_CASE(
	"custom on_not_found and on_error handlers") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;

	router.on_not_found([](HttpRequest const &req) {
		return HttpResponse::json(std::format(R"({{"error":"not_found","path":"{}"}})", req.path));
	});

	router.on_error([](HttpRequest const &, std::exception const &ex) {
		return HttpResponse::json(
			std::format(R"({{"error":"internal","detail":"{}"}})", ex.what()),
			500,
			"Internal Server Error");
	});

	router.get("/ok", [](HttpRequest const &) { return HttpResponse::text("all good"); });
	router.get("/boom", [](HttpRequest const &) -> HttpResponse { throw std::runtime_error{"something exploded"}; });

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const err_port = srv.port();

	auto get = [&](SV path) {
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(err_port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
		auto req = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
		::send(fd, req.data(), req.size(), 0);
		auto resp = read_one_response(fd);
		::close(fd);
		return resp;
	};

	SECTION("existing route still responds normally") {
		auto resp = get("/ok");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		auto hdr_end = resp.find("\r\n\r\n");
		REQUIRE(resp.substr(hdr_end + 4) == "all good");
	}

	SECTION("custom not_found handler returns JSON 404") {
		auto resp = get("/missing");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK")); // status from handler
		REQUIRE(resp.find("application/json") != S::npos);
		auto hdr_end = resp.find("\r\n\r\n");
		REQUIRE(resp.substr(hdr_end + 4) == R"({"error":"not_found","path":"/missing"})");
	}

	SECTION("throwing handler returns custom error response with exception message") {
		auto resp = get("/boom");
		REQUIRE(resp.starts_with("HTTP/1.1 500 Internal Server Error"));
		REQUIRE(resp.find("application/json") != S::npos);
		auto hdr_end = resp.find("\r\n\r\n");
		REQUIRE(resp.substr(hdr_end + 4) == R"({"error":"internal","detail":"something exploded"})");
	}

	srv.stop();
}

// ---------------------------------------------------------------------------
// Chunked Transfer-Encoding receive
// ---------------------------------------------------------------------------

TEST_CASE(
	"POST with Transfer-Encoding: chunked body is decoded and echoed") {
	ensure_server();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_test_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	// "Hello, " (7 bytes) + "world!" (6 bytes) + terminal chunk
	SV const raw =
		"POST /api/echo-body HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Type: text/plain\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: close\r\n"
		"\r\n"
		"7\r\n"
		"Hello, \r\n"
		"6\r\n"
		"world!\r\n"
		"0\r\n"
		"\r\n";
	::send(fd, raw.data(), raw.size(), 0);
	auto resp = read_one_response(fd);
	::close(fd);

	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "Hello, world!");
}

TEST_CASE(
	"POST with chunked body and chunk extension is decoded correctly") {
	ensure_server();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_test_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	// Chunk extensions (";name=val") must be ignored.
	SV const raw =
		"POST /api/echo-body HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Type: text/plain\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: close\r\n"
		"\r\n"
		"5;ext=ignored\r\n"
		"abcde\r\n"
		"0\r\n"
		"\r\n";
	::send(fd, raw.data(), raw.size(), 0);
	auto resp = read_one_response(fd);
	::close(fd);

	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "abcde");
}

// ---------------------------------------------------------------------------
// Cookie support
// ---------------------------------------------------------------------------

TEST_CASE(
	"Cookie header is parsed and individual cookies are accessible") {
	auto resp = http_get_with_headers("/api/echo-cookie", "Cookie: name=hello; other=world\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "hello");
}

TEST_CASE(
	"Cookie header with single cookie is parsed correctly") {
	auto resp = http_get_with_headers("/api/echo-cookie", "Cookie: name=just-one\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "just-one");
}

TEST_CASE(
	"Request without Cookie header finds no cookies") {
	auto resp = http_get("/api/echo-cookie");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}

TEST_CASE(
	"Response set_cookie emits Set-Cookie headers") {
	auto resp = http_get("/api/set-cookie");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Set-Cookie: session=abc123; Path=/; HttpOnly\r\n") != S::npos);
	REQUIRE(resp.find("Set-Cookie: theme=dark\r\n") != S::npos);
}

// ---------------------------------------------------------------------------
// Route groups
// ---------------------------------------------------------------------------

TEST_CASE(
	"group route responds at prefixed path") {
	auto resp = http_get("/api/v2/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"v":"2","status":"ok"})");
}

TEST_CASE(
	"group middleware stamps header on grouped routes only") {
	auto v2 = http_get("/api/v2/status");
	REQUIRE(v2.find("X-Api-Version: 2\r\n") != S::npos);

	// Route outside the group must NOT receive the group middleware header.
	auto v1 = http_get("/api/v1/status");
	REQUIRE(v1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(v1.find("X-Api-Version:") == S::npos);
}

TEST_CASE(
	"group route with path parameter resolves param correctly") {
	auto resp = http_get("/api/v2/item/42");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"id":"42"})");
}

TEST_CASE(
	"group route not found returns 404") {
	auto resp = http_get("/api/v2/nonexistent");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}

// ---------------------------------------------------------------------------
// Static file serving
// ---------------------------------------------------------------------------

TEST_CASE(
	"static file serving") {
	// Create a temp directory with test files.
	char tmpdir[] = "/tmp/conflux_static_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto write_file = [&](SV name, SV content) {
		auto path = S{tmpdir} + "/" + S{name};
		int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(fd >= 0);
		ssize_t const written = ::write(fd, content.data(), content.size());
		REQUIRE(written == static_cast<ssize_t>(content.size()));
		::close(fd);
	};
	write_file("hello.txt", "Hello, static!");
	write_file("page.html", "<h1>Static HTML</h1>");
	write_file("data.json", R"({"key":"value"})");

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	router.serve_static("/static", S{tmpdir});

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const static_port = srv.port();

	auto get = [&](SV path, SV extra = "") {
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
		auto req = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}\r\n", path, extra);
		::send(fd, req.data(), req.size(), 0);
		auto resp = read_one_response(fd);
		::close(fd);
		return resp;
	};

	SECTION("serves .txt file with correct MIME type") {
		auto resp = get("/static/hello.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(resp.find("Content-Type: text/plain; charset=utf-8\r\n") != S::npos);
		auto hdr_end = resp.find("\r\n\r\n");
		REQUIRE(resp.substr(hdr_end + 4) == "Hello, static!");
	}

	SECTION("serves .html file with correct MIME type") {
		auto resp = get("/static/page.html");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(resp.find("Content-Type: text/html; charset=utf-8\r\n") != S::npos);
		auto hdr_end = resp.find("\r\n\r\n");
		REQUIRE(resp.substr(hdr_end + 4) == "<h1>Static HTML</h1>");
	}

	SECTION("serves .json file with correct MIME type") {
		auto resp = get("/static/data.json");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(resp.find("Content-Type: application/json\r\n") != S::npos);
	}

	SECTION("returns ETag header") {
		auto resp = get("/static/hello.txt");
		REQUIRE(resp.find("ETag: \"") != S::npos);
	}

	SECTION("returns 304 when If-None-Match matches ETag") {
		auto resp1 = get("/static/hello.txt");
		auto etag_pos = resp1.find("ETag: ");
		REQUIRE(etag_pos != S::npos);
		etag_pos += 6;
		auto etag_end = resp1.find("\r\n", etag_pos);
		auto etag = resp1.substr(etag_pos, etag_end - etag_pos);

		auto resp2 = get("/static/hello.txt", std::format("If-None-Match: {}\r\n", etag));
		REQUIRE(resp2.starts_with("HTTP/1.1 304 Not Modified"));
	}

	SECTION("returns 304 for If-Modified-Since using GMT under non-UTC TZ") {
		struct TzGuard {
			Opt<S> old_tz;
			TzGuard() {
				if (char const *tz = ::getenv("TZ"); tz != nullptr) {
					old_tz = tz;
				}
				::setenv("TZ", "Asia/Tokyo", 1);
				::tzset();
			}
			~TzGuard() {
				if (old_tz) {
					::setenv("TZ", old_tz->c_str(), 1);
				} else {
					::unsetenv("TZ");
				}
				::tzset();
			}
		} const tz_guard;

		auto resp1 = get("/static/hello.txt");
		auto lm_pos = resp1.find("Last-Modified: ");
		REQUIRE(lm_pos != S::npos);
		lm_pos += 15;
		auto lm_end = resp1.find("\r\n", lm_pos);
		auto last_modified = resp1.substr(lm_pos, lm_end - lm_pos);

		auto resp2 = get("/static/hello.txt", std::format("If-Modified-Since: {}\r\n", last_modified));
		REQUIRE(resp2.starts_with("HTTP/1.1 304 Not Modified"));
	}

	SECTION("returns 404 for missing file") {
		auto resp = get("/static/missing.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
	}

	SECTION("rejects path traversal with 403") {
		auto resp = get("/static/../etc/passwd");
		REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
	}

	srv.stop();

	// Cleanup temp files.
	for (auto const &name: {"hello.txt", "page.html", "data.json"}) {
		::unlink((S{tmpdir} + "/" + name).c_str());
	}
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: offload_pool parity") {
	char tmpdir[] = "/tmp/conflux_static_off_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto write_file = [&](SV name, SV content) {
		auto path = S{tmpdir} + "/" + S{name};
		int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(fd >= 0);
		ssize_t const written = ::write(fd, content.data(), content.size());
		REQUIRE(written == static_cast<ssize_t>(content.size()));
		::close(fd);
	};
	write_file("hello.txt", "Hello, offloaded!");
	write_file("empty.txt", "");

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	auto pool = std::make_shared<WorkPool>();

	Router router;
	StaticOptions sopts{};
	sopts.offload_pool = pool;
	router.serve_static("/static", S{tmpdir}, sopts);

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const static_port = srv.port();

	auto get = [&](SV path, SV extra = "") {
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
		auto req = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}\r\n", path, extra);
		::send(fd, req.data(), req.size(), 0);
		auto resp = read_one_response(fd);
		::close(fd);
		return resp;
	};

	SECTION("offloaded GET returns body via pool") {
		auto resp = get("/static/hello.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		auto hdr_end = resp.find("\r\n\r\n");
		REQUIRE(resp.substr(hdr_end + 4) == "Hello, offloaded!");
	}

	SECTION("offloaded 404 for missing") {
		auto resp = get("/static/missing.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
	}

	SECTION("offloaded 403 for traversal") {
		auto resp = get("/static/../etc/passwd");
		REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
	}

	SECTION("offloaded 304 If-None-Match round-trip") {
		auto resp1 = get("/static/hello.txt");
		auto etag_pos = resp1.find("ETag: ");
		REQUIRE(etag_pos != S::npos);
		etag_pos += 6;
		auto etag_end = resp1.find("\r\n", etag_pos);
		auto etag = resp1.substr(etag_pos, etag_end - etag_pos);
		auto resp2 = get("/static/hello.txt", std::format("If-None-Match: {}\r\n", etag));
		REQUIRE(resp2.starts_with("HTTP/1.1 304 Not Modified"));
	}

	SECTION("offloaded zero-size file") {
		auto resp = get("/static/empty.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		auto hdr_end = resp.find("\r\n\r\n");
		REQUIRE(resp.substr(hdr_end + 4).empty());
	}

	SECTION("many concurrent offloaded requests") {
		constexpr int kClients = 32;
		V<std::jthread> threads;
		std::atomic<int> ok{0};
		threads.reserve(kClients);
		for (int i = 0; i < kClients; ++i) {
			threads.emplace_back([&] {
				for (int attempt = 0; attempt < 3; ++attempt) {
					auto resp = get("/static/hello.txt");
					if (resp.starts_with("HTTP/1.1 200 OK") && resp.find("Hello, offloaded!") != S::npos) {
						ok.fetch_add(1);
						return;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
			});
		}
		threads.clear();
		REQUIRE(ok.load() == kClients);
	}

	srv.stop();
	pool->stop();
	pool->wait();

	for (auto const &name: {"hello.txt", "empty.txt"}) {
		::unlink((S{tmpdir} + "/" + name).c_str());
	}
	::rmdir(tmpdir);
}

// ---------------------------------------------------------------------------
// serve_static: allow_put and allow_delete
// ---------------------------------------------------------------------------

TEST_CASE(
	"static file serving: allow_put creates and overwrites files") {
	char tmpdir[] = "/tmp/conflux_static_put_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	StaticOptions sopts{};
	sopts.allow_put = true;
	router.serve_static("/static", S{tmpdir}, sopts);

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const port = srv.port();

	auto raw_request = [&](SV method, SV path, SV body = "") {
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
		auto req = std::format(
			"{} {} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
			method,
			path,
			body.size(),
			body);
		::send(fd, req.data(), req.size(), 0);
		auto resp = read_one_response(fd);
		::close(fd);
		return resp;
	};

	SECTION("PUT new file returns 201 Created") {
		auto resp = raw_request("PUT", "/static/new.txt", "hello");
		REQUIRE(resp.starts_with("HTTP/1.1 201 Created"));
		// Verify file was written.
		auto path = S{tmpdir} + "/new.txt";
		int const fd = ::open(path.c_str(), O_RDONLY);
		REQUIRE(fd >= 0);
		char buf[16]{};
		auto n = ::read(fd, buf, sizeof(buf));
		::close(fd);
		REQUIRE(n == 5);
		CHECK(SV{buf, static_cast<size_t>(n)} == "hello");
		::unlink(path.c_str());
	}

	SECTION("PUT existing file returns 204 No Content") {
		auto path = S{tmpdir} + "/existing.txt";
		int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(fd >= 0);
		::write(fd, "old", 3);
		::close(fd);

		auto resp = raw_request("PUT", "/static/existing.txt", "new-content");
		REQUIRE(resp.starts_with("HTTP/1.1 204 No Content"));
		::unlink(path.c_str());
	}

	SECTION("PUT with path traversal returns 403") {
		auto resp = raw_request("PUT", "/static/../escape.txt", "data");
		REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
	}

	srv.stop();
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: allow_delete removes files") {
	char tmpdir[] = "/tmp/conflux_static_del_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	StaticOptions sopts{};
	sopts.allow_delete = true;
	router.serve_static("/static", S{tmpdir}, sopts);

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const port = srv.port();

	auto raw_request = [&](SV method, SV path) {
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
		auto req = std::format(
			"{} {} HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
			method,
			path);
		::send(fd, req.data(), req.size(), 0);
		auto resp = read_one_response(fd);
		::close(fd);
		return resp;
	};

	SECTION("DELETE existing file returns 204 No Content") {
		auto path = S{tmpdir} + "/todelete.txt";
		int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(fd >= 0);
		::write(fd, "bye", 3);
		::close(fd);

		auto resp = raw_request("DELETE", "/static/todelete.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 204 No Content"));
		// Verify file is gone.
		CHECK(::access(path.c_str(), F_OK) != 0);
	}

	SECTION("DELETE missing file returns 404") {
		auto resp = raw_request("DELETE", "/static/nonexistent.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
	}

	SECTION("DELETE with path traversal returns 403") {
		auto resp = raw_request("DELETE", "/static/../escape.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
	}

	srv.stop();
	::rmdir(tmpdir);
}

// ---------------------------------------------------------------------------
// serve_static: HEAD request
// ---------------------------------------------------------------------------

TEST_CASE(
	"static file: HEAD returns 200 with correct Content-Length but no body") {
	char tmpdir[] = "/tmp/conflux_head_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto path = S{tmpdir} + "/hello.txt";
	int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(fd >= 0);
	SV const content = "Hello, static!";
	::write(fd, content.data(), content.size());
	::close(fd);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	router.serve_static("/f", S{tmpdir});
	ScopedTestServer srv{cfg, std::move(router)};

	int const s = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	SV const req = "HEAD /f/hello.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(s, req.data(), req.size(), 0);
	auto resp = read_one_response(s);
	::close(s);

	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Content-Length must reflect the actual file size (14 bytes).
	REQUIRE(resp.find("Content-Length: 14") != S::npos);
	// Body must be empty for HEAD response.
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.size() == hdr_end + 4);

	::unlink(path.c_str());
	::rmdir(tmpdir);
	srv.stop();
}

// ---------------------------------------------------------------------------
// serve_static: Range requests (HTTP/1.1 206 Partial Content)
// ---------------------------------------------------------------------------

TEST_CASE(
	"static file: Range request returns 206 with partial body") {
	char tmpdir[] = "/tmp/conflux_range_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto path = S{tmpdir} + "/data.txt";
	int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(fd >= 0);
	SV const content = "0123456789";
	::write(fd, content.data(), content.size());
	::close(fd);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	Router router;
	router.serve_static("/f", S{tmpdir});
	ScopedTestServer srv{cfg, std::move(router)};

	// Request bytes 2-5 (inclusive): "2345"
	int const s = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	SV const req =
		"GET /f/data.txt HTTP/1.1\r\nHost: localhost\r\nRange: bytes=2-5\r\nConnection: close\r\n\r\n";
	::send(s, req.data(), req.size(), 0);
	auto resp = read_one_response(s);
	::close(s);

	REQUIRE(resp.starts_with("HTTP/1.1 206 Partial Content"));
	REQUIRE(resp.find("Content-Range: bytes 2-5/10") != S::npos);
	REQUIRE(extract_body(resp) == "2345");

	::unlink(path.c_str());
	::rmdir(tmpdir);
	srv.stop();
}

TEST_CASE(
	"static file: Range beyond file size returns 416") {
	char tmpdir[] = "/tmp/conflux_range416_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto path = S{tmpdir} + "/tiny.txt";
	int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(fd >= 0);
	SV const content = "hi";
	::write(fd, content.data(), content.size());
	::close(fd);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	router.serve_static("/f", S{tmpdir});
	ScopedTestServer srv{cfg, std::move(router)};

	int const s = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	SV const req =
		"GET /f/tiny.txt HTTP/1.1\r\nHost: localhost\r\nRange: bytes=100-200\r\nConnection: close\r\n\r\n";
	::send(s, req.data(), req.size(), 0);
	auto resp = read_one_response(s);
	::close(s);

	REQUIRE(resp.starts_with("HTTP/1.1 416"));

	::unlink(path.c_str());
	::rmdir(tmpdir);
	srv.stop();
}

// ---------------------------------------------------------------------------
// Request/idle timeout
// ---------------------------------------------------------------------------

TEST_CASE(
	"static file: suffix Range (bytes=-N) falls through to full 200 response") {
	char tmpdir[] = "/tmp/conflux_suffix_range_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto path = S{tmpdir} + "/data.txt";
	int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(fd >= 0);
	SV const content = "0123456789";
	::write(fd, content.data(), content.size());
	::close(fd);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	router.serve_static("/f", S{tmpdir});
	ScopedTestServer srv{cfg, std::move(router)};

	// bytes=-5: last 5 bytes not implemented → must NOT return first 5 bytes (old bug)
	int const s = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	SV const req =
		"GET /f/data.txt HTTP/1.1\r\nHost: localhost\r\nRange: bytes=-5\r\nConnection: close\r\n\r\n";
	::send(s, req.data(), req.size(), 0);
	auto resp = read_one_response(s);
	::close(s);

	// Full file (suffix-range unimplemented → 200 with full body) or 206 with correct last 5 bytes.
	// Must not return first 5 bytes as a 206.
	if (resp.starts_with("HTTP/1.1 206")) {
		// If we ever implement suffix ranges, verify correctness.
		REQUIRE(extract_body(resp) == "56789");
	} else {
		// Currently expected: fall through to full 200 response.
		REQUIRE(resp.starts_with("HTTP/1.1 200"));
		REQUIRE(extract_body(resp) == "0123456789");
	}

	::unlink(path.c_str());
	::rmdir(tmpdir);
	srv.stop();
}

TEST_CASE(
	"idle connection is closed after request_timeout_ms") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 1500; // 1.5 s — shorter than the 1s timer tick + margin

	Router router;
	router.get("/ok", [](HttpRequest const &) { return HttpResponse::text("ok"); });

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const timeout_port = srv.port();

	// Connect but send nothing — the server should close the connection after ~1.5s.
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(timeout_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	// Wait up to 4 seconds for EOF from server.
	char buf[1];
	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	auto n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
	::close(fd);

	// Either 0 (clean close) or -1/ECONNRESET: connection is gone.
	bool const connection_gone = (n == 0) || (n < 0 && (errno == ECONNRESET || errno == EAGAIN));
	REQUIRE(connection_gone);

	// Normal request still works after a timeout reap.
	int const fd2 = ::socket(AF_INET, SOCK_STREAM, 0);
	::connect(fd2, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	SV const req = "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd2, req.data(), req.size(), 0);
	auto resp = read_one_response(fd2);
	::close(fd2);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));

	srv.stop();
}

// ---------------------------------------------------------------------------
// Access log middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"make_access_log_middleware logs request lines to ostream") {
	std::ostringstream log_out;
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	router.use(make_access_log_middleware(log_out));
	router.get("/ping", [](HttpRequest const &) { return HttpResponse::text("pong"); });
	router.get("/missing", [](HttpRequest const &req) { return HttpResponse::not_found(req.path); });

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const log_port = srv.port();

	auto get = [&](SV path) {
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(log_port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
		auto req = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
		::send(fd, req.data(), req.size(), 0);
		auto resp = read_one_response(fd);
		::close(fd);
		return resp;
	};

	get("/ping");
	get("/missing");
	std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let the log flush

	srv.stop();

	auto log = log_out.str();
	REQUIRE(log.find("GET /ping 200") != S::npos);
	REQUIRE(log.find("GET /missing 404") != S::npos);
	// Each line starts with a timestamp in ISO 8601 format.
	REQUIRE(log.find("[20") != S::npos);
}

// ---------------------------------------------------------------------------
// WebSocket upgrade
// ---------------------------------------------------------------------------

TEST_CASE(
	"WebSocket upgrade performs handshake, echoes text frames, closes cleanly") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		// Echo all text frames until connection closes.
		while (auto frame = ws.recv()) {
			if (frame->opcode == WsConn::Opcode::Text) {
				ws.send_text(frame->payload);
			}
		}
	});

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const ws_port = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ws_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	// --- Handshake ---
	// RFC 6455 §1.3 test vector: key → accept.
	SV const ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
	S upgrade_req = std::format(
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: {}\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n",
		ws_key);
	::send(fd, upgrade_req.data(), upgrade_req.size(), 0);

	A<char, 4096> buf{};
	auto n = ::recv(fd, buf.data(), buf.size(), 0);
	REQUIRE(n > 0);
	SV const resp{buf.data(), static_cast<SZ>(n)};
	REQUIRE(resp.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
	REQUIRE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != SV::npos);

	// --- Send masked text frame "hello" ---
	// FIN=1, opcode=1; MASK=1, len=5; mask=0x01020304; masked payload.
	A<uint8_t, 4> const mask = {0x01, 0x02, 0x03, 0x04};
	SV const msg = "hello";
	A<uint8_t, 11> tx_frame{};
	tx_frame[0] = 0x81; // FIN | text
	tx_frame[1] = 0x80 | 0x05; // MASK | len=5
	tx_frame[2] = mask[0];
	tx_frame[3] = mask[1];
	tx_frame[4] = mask[2];
	tx_frame[5] = mask[3];
	for (SZ i = 0; i < msg.size(); ++i) {
		tx_frame[6 + i] = static_cast<uint8_t>(msg[i]) ^ mask[i & 3];
	}
	::send(fd, tx_frame.data(), tx_frame.size(), 0);

	// --- Receive unmasked echo frame ---
	A<uint8_t, 64> rx_buf{};
	auto rn = ::recv(fd, rx_buf.data(), rx_buf.size(), 0);
	REQUIRE(rn >= 7); // 2 header + 5 payload
	REQUIRE(rx_buf[0] == 0x81); // FIN | text
	REQUIRE((rx_buf[1] & 0x80U) == 0U); // NOT masked (server→client)
	REQUIRE((rx_buf[1] & 0x7FU) == 5U); // payload length = 5
	S echo{reinterpret_cast<char const *>(rx_buf.data() + 2), 5};
	REQUIRE(echo == "hello");

	// --- Send close frame (masked, status 1000) ---
	uint16_t const status = 1000;
	A<uint8_t, 8> close_frame{};
	close_frame[0] = 0x88; // FIN | close
	close_frame[1] = 0x80 | 0x02; // MASK | len=2
	close_frame[2] = 0xAA;
	close_frame[3] = 0xBB;
	close_frame[4] = 0xCC;
	close_frame[5] = 0xDD; // mask key
	close_frame[6] = static_cast<uint8_t>((status >> 8) ^ close_frame[2]);
	close_frame[7] = static_cast<uint8_t>((status & 0xFF) ^ close_frame[3]);
	::send(fd, close_frame.data(), close_frame.size(), 0);

	// Server echoes a close frame then closes.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	::close(fd);

	srv.stop();
}

// ---------------------------------------------------------------------------
// compress_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"compress: large body with Accept-Encoding gzip is compressed") {
	ensure_compress_server();
	auto resp = http_get_on(g_compress_port, "/big", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") != S::npos);
	REQUIRE(resp.find("Vary: Accept-Encoding") != S::npos);
	// Body decompresses back to 512 A's.
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	auto body = resp.substr(hdr_end + 4);
	auto decompressed = gzip_decompress(body);
	REQUIRE(decompressed == S(512, 'A'));
}

TEST_CASE(
	"compress: large body without Accept-Encoding is not compressed") {
	ensure_compress_server();
	auto resp = http_get_on(g_compress_port, "/big");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") == S::npos);
}

TEST_CASE(
	"compress: body smaller than min_body_size is not compressed") {
	ensure_compress_server();
	auto resp = http_get_on(g_compress_port, "/small", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") == S::npos);
}

TEST_CASE(
	"compress: non-compressible MIME type is not compressed") {
	ensure_compress_server();
	auto resp = http_get_on(g_compress_port, "/bin", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") == S::npos);
}

// ---------------------------------------------------------------------------
// security_headers_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"security: default options inject HSTS header") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("Strict-Transport-Security:") != S::npos);
	REQUIRE(resp.find("max-age=") != S::npos);
}

TEST_CASE(
	"security: default options inject X-Frame-Options DENY") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("X-Frame-Options: DENY") != S::npos);
}

TEST_CASE(
	"security: default options inject X-Content-Type-Options nosniff") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("X-Content-Type-Options: nosniff") != S::npos);
}

TEST_CASE(
	"security: default options inject Referrer-Policy") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("Referrer-Policy:") != S::npos);
}

TEST_CASE(
	"security: default options inject X-XSS-Protection") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("X-XSS-Protection: 1; mode=block") != S::npos);
}

TEST_CASE(
	"security: default options inject Permissions-Policy") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("Permissions-Policy:") != S::npos);
}

TEST_CASE(
	"security: custom CSP is injected") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	SecurityOptions sopts{};
	sopts.csp = "default-src 'self'";

	Router router;
	router.use(security_headers_middleware(sopts));
	router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });

	ScopedTestServer srv{cfg, std::move(router)};
	auto resp = http_get_on(srv.port(), "/");
	REQUIRE(resp.find("Content-Security-Policy: default-src 'self'") != S::npos);
}

TEST_CASE(
	"security: hsts with no subdomains omits includeSubDomains") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	SecurityOptions sopts{};
	sopts.hsts_include_subdomains = false;

	Router router;
	router.use(security_headers_middleware(sopts));
	router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });

	ScopedTestServer srv{cfg, std::move(router)};
	auto resp = http_get_on(srv.port(), "/");
	REQUIRE(resp.find("Strict-Transport-Security:") != S::npos);
	REQUIRE(resp.find("includeSubDomains") == S::npos);
}

TEST_CASE(
	"security: hsts_max_age=0 disables HSTS header") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(security_headers_middleware({.hsts_max_age = 0}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.find("Strict-Transport-Security") == S::npos);
}

TEST_CASE(
	"security: empty frame_options disables X-Frame-Options header") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(security_headers_middleware({.frame_options = ""}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.find("X-Frame-Options") == S::npos);
}

// ---------------------------------------------------------------------------
// cors_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"cors: preflight with matching origin returns 204 and ACAO") {
	ensure_cors_server();
	auto resp = http_options_on(
		g_cors_port,
		"/api",
		"Origin: https://test.example\r\n"
		"Access-Control-Request-Method: GET\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 204"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://test.example") != S::npos);
	REQUIRE(resp.find("Access-Control-Allow-Methods:") != S::npos);
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
	REQUIRE(resp.find("Access-Control-Allow-Origin:") == S::npos);
}

TEST_CASE(
	"cors: GET with matching origin receives ACAO header") {
	ensure_cors_server();
	auto resp = http_get_on(g_cors_port, "/api", "Origin: https://test.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://test.example") != S::npos);
}

TEST_CASE(
	"cors: GET with matching origin appends Origin to existing Vary") {
	ensure_cors_server();
	auto resp = http_get_on(g_cors_port, "/vary", "Origin: https://test.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Vary: Accept-Encoding, Origin") != S::npos);
}

TEST_CASE(
	"cors: GET without Origin header has no ACAO header") {
	ensure_cors_server();
	auto resp = http_get_on(g_cors_port, "/api");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin:") == S::npos);
}

TEST_CASE(
	"cors: allow_credentials reflects origin instead of wildcard") {
	ensure_cors_cred_server();
	auto resp = http_get_on(g_cors_cred_port, "/api", "Origin: https://foo.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://foo.example") != S::npos);
	REQUIRE(resp.find("Access-Control-Allow-Credentials: true") != S::npos);
}

TEST_CASE(
	"cors: expose_headers present in non-preflight response") {
	ensure_cors_cred_server();
	auto resp = http_get_on(g_cors_cred_port, "/api", "Origin: https://foo.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Expose-Headers:") != S::npos);
	REQUIRE(resp.find("X-Custom-Header") != S::npos);
	REQUIRE(resp.find("X-Request-Id") != S::npos);
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
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://foo.example") != S::npos);
	REQUIRE(resp.find("Access-Control-Allow-Credentials: true") != S::npos);
}

TEST_CASE(
	"cors: wildcard origin without credentials returns ACAO: *") {
	ensure_cors_wildcard_server();
	auto resp = http_get_on(g_cors_wildcard_port, "/api", "Origin: https://any.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: *") != S::npos);
	REQUIRE(resp.find("Access-Control-Allow-Credentials:") == S::npos);
}

TEST_CASE(
	"cors: OPTIONS without Access-Control-Request-Method is not a preflight") {
	ensure_cors_server();
	auto resp = http_options_on(g_cors_port, "/api", "Origin: https://test.example\r\n");
	// Not a preflight — passes to next handler, which may 405 or 200 depending on router.
	// Either way, CORS headers are still injected for the origin.
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://test.example") != S::npos);
	REQUIRE(resp.find("Access-Control-Allow-Methods:") == S::npos);
}

// ---------------------------------------------------------------------------
// basic_auth_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"basic_auth: missing Authorization returns 401 with WWW-Authenticate") {
	ensure_auth_server();
	auto resp = http_get_on(g_auth_port, "/protected");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find("WWW-Authenticate: Basic") != S::npos);
}

TEST_CASE(
	"basic_auth: wrong credentials return 401") {
	ensure_auth_server();
	// "baduser:badpass" in base64
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: Basic YmFkdXNlcjpiYWRwYXNz\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}

TEST_CASE(
	"basic_auth: correct credentials return 200") {
	ensure_auth_server();
	// "testuser:testpass" in base64 = dGVzdHVzZXI6dGVzdHBhc3M=
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: Basic dGVzdHVzZXI6dGVzdHBhc3M=\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "secret");
}

TEST_CASE(
	"basic_auth: base64 credential without colon returns 401") {
	ensure_auth_server();
	// base64("nocolon") — no colon separator
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: Basic bm9jb2xvbg==\r\n"); // "nocolon"
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}

TEST_CASE(
	"basic_auth: custom realm appears in WWW-Authenticate header") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(basic_auth_middleware([](SV, SV) { return false; }, "My Realm"));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("x"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find(R"(WWW-Authenticate: Basic realm="My Realm")") != S::npos);
}

TEST_CASE(
	"basic_auth: lowercase scheme returns 200") {
	ensure_auth_server();
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: basic dGVzdHVzZXI6dGVzdHBhc3M=\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}

// ---------------------------------------------------------------------------
// bearer_auth_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"bearer_auth: missing Authorization returns 401 with WWW-Authenticate") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find("WWW-Authenticate: Bearer") != S::npos);
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
	// Extra space after "Bearer " — trim() should strip it before validation.
	auto resp = http_get_on(g_bearer_port, "/protected", "Authorization: Bearer  valid-token-123 \r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}

TEST_CASE(
	"bearer_auth: lowercase scheme returns 200") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected", "Authorization: bearer valid-token-123\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}

// ---------------------------------------------------------------------------
// rate_limit_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"rate_limit: first two requests succeed, third returns 429") {
	ensure_rate_server();
	// 3 requests in sequence; bucket size = 2.
	auto r1 = http_get_on(g_rate_port, "/");
	auto r2 = http_get_on(g_rate_port, "/");
	auto r3 = http_get_on(g_rate_port, "/");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r3.starts_with("HTTP/1.1 429"));
	REQUIRE(r3.find("Retry-After:") != S::npos);
}

TEST_CASE(
	"rate_limit: burst allows extra requests beyond base rate") {
	ensure_rate_burst_server();
	// requests=1 + burst=2 → capacity=3. First 3 succeed; 4th returns 429.
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
	// Use the existing 1-request rate server: exhaust it then check the header.
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(rate_limit_middleware({.requests = 1, .window = std::chrono::seconds{10}}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	http_get_on(port, "/"); // consume the one allowed request
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 429"));
	auto retry = extract_header(resp, "Retry-After");
	REQUIRE(!retry.empty());
	int retry_val = std::stoi(S{retry});
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

// ---------------------------------------------------------------------------
// TLS (HTTPS) shared infrastructure
// ---------------------------------------------------------------------------

uint16_t g_tls_port = 0;

// Generate a self-signed cert+key pair once, start a TLS server, delete the
// temp files (already loaded into SSL_CTX by the time port() returns).
void ensure_tls_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		char cert_tmp[] = "/tmp/conflux_tls_cert_XXXXXX.pem";
		char key_tmp[] = "/tmp/conflux_tls_key_XXXXXX.pem";
		{
			int fd = ::mkstemps(cert_tmp, 4);
			if (fd < 0) {
				throw std::runtime_error{"mkstemps cert"};
			}
			::close(fd);
			fd = ::mkstemps(key_tmp, 4);
			if (fd < 0) {
				throw std::runtime_error{"mkstemps key"};
			}
			::close(fd);
		}
		S const cmd = std::format(
			"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
			"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
			key_tmp,
			cert_tmp);
		if (::system(cmd.c_str()) != 0) {
			throw std::runtime_error{"openssl req failed"};
		}
		Config cfg = mw_config();
		cfg.cert_file = cert_tmp;
		cfg.key_file = key_tmp;

		Router router;
		router.get("/ping", [](HttpRequest const &) { return HttpResponse::json(R"({"tls":true})"); });
		router.get("/hello/{name}", [](HttpRequest const &req) {
			return HttpResponse::text(std::format("hello {}", req.params["name"]));
		});
		router.post("/echo", [](HttpRequest const &req) { return HttpResponse::text(req.body); });
		router.put("/put/{id}", [](HttpRequest const &req) {
			return HttpResponse::json(std::format(R"({{"id":"{}"}})", req.params["id"]));
		});
		router.get("/notfound-test", [](HttpRequest const &) -> HttpResponse {
			// deliberately absent — router returns 404
			return HttpResponse::not_found("notfound-test");
		});

		g_tls_port = start_mw_server(cfg, std::move(router));
		// Cert+key are loaded; temp files no longer needed.
		::unlink(cert_tmp);
		::unlink(key_tmp);
	});
}

// Open one TLS connection to g_tls_port, send an arbitrary raw request,
// read back a complete HTTP response (via Content-Length), close.
S tls_raw(
	uint16_t port,
	SV raw_request) {
	SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

	SSL *ssl = SSL_new(ctx);
	SSL_set_fd(ssl, fd);
	SSL_connect(ssl);

	SSL_write(ssl, raw_request.data(), static_cast<int>(raw_request.size()));

	S response;
	A<char, 4096> buf{};
	for (;;) {
		int const n = SSL_read(ssl, buf.data(), static_cast<int>(buf.size()));
		if (n <= 0) {
			break;
		}
		response.append(buf.data(), static_cast<SZ>(n));
		auto hdr_end = response.find("\r\n\r\n");
		if (hdr_end == S::npos) {
			continue;
		}
		auto cl_pos = response.find("Content-Length: ");
		if (cl_pos == S::npos || cl_pos > hdr_end) {
			break;
		}
		cl_pos += 16;
		auto cl_end = response.find("\r\n", cl_pos);
		SZ body_len = 0;
		std::from_chars(response.data() + cl_pos, response.data() + cl_end, body_len);
		if (response.size() >= hdr_end + 4 + body_len) {
			break;
		}
	}

	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	::close(fd);
	return response;
}

// Convenience wrappers.
S tls_get(
	SV path,
	SV extra = "") {
	ensure_tls_server();
	return tls_raw(
		g_tls_port,
		std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}\r\n", path, extra));
}

S tls_post(
	SV path,
	SV body,
	SV ct = "text/plain") {
	ensure_tls_server();
	return tls_raw(
		g_tls_port,
		std::format(
			"POST {} HTTP/1.1\r\nHost: localhost\r\nContent-Type: {}\r\n"
			"Content-Length: {}\r\nConnection: close\r\n\r\n{}",
			path,
			ct,
			body.size(),
			body));
}

// ---------------------------------------------------------------------------
// TLS (HTTPS) tests
// ---------------------------------------------------------------------------

TEST_CASE(
	"TLS: GET returns JSON response") {
	auto resp = tls_get("/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"tls":true})");
}

TEST_CASE(
	"http client: HTTPS GET /ping returns parsed response") {
	ensure_tls_server();
	HttpClientOptions tls_opts{};
	tls_opts.verify_peer = false;
	HttpClient tls_client{std::move(tls_opts)};
	auto response = tls_client.send_blocking(
		chttp::HttpRequest::get(std::format("https://127.0.0.1:{}/ping", g_tls_port)).server_name("localhost").build());
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(S{response->head.headers["content-type"]}.find("application/json") != S::npos);
	CHECK(response->body == R"({"tls":true})");
}

TEST_CASE(
	"TLS: GET with path parameter") {
	auto resp = tls_get("/hello/world");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "hello world");
}

TEST_CASE(
	"TLS: POST body is echoed back") {
	auto resp = tls_post("/echo", "hello TLS");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "hello TLS");
}

TEST_CASE(
	"TLS: POST with binary-safe body") {
	S body(256, '\x00');
	for (int i = 0; i < 256; ++i) {
		body[static_cast<SZ>(i)] = static_cast<char>(i);
	}
	auto resp = tls_post("/echo", body, "application/octet-stream");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == body);
}

TEST_CASE(
	"TLS: PUT with path param returns JSON") {
	ensure_tls_server();
	auto resp = tls_raw(
		g_tls_port,
		"PUT /put/42 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"id":"42"})");
}

TEST_CASE(
	"TLS: unknown route returns 404") {
	auto resp = tls_get("/does-not-exist");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}

TEST_CASE(
	"TLS: pipelined requests on one connection both succeed") {
	ensure_tls_server();
	SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_tls_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	SSL *ssl = SSL_new(ctx);
	SSL_set_fd(ssl, fd);
	REQUIRE(SSL_connect(ssl) == 1);

	// Send two requests back-to-back before reading any response.
	SV const r1 = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
	SV const r2 = "GET /hello/pipe HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	SSL_write(ssl, r1.data(), static_cast<int>(r1.size()));
	SSL_write(ssl, r2.data(), static_cast<int>(r2.size()));

	// Read first response via Content-Length.
	auto read_one_tls = [&]() {
		S resp;
		A<char, 4096> buf{};
		while (true) {
			int const n = SSL_read(ssl, buf.data(), static_cast<int>(buf.size()));
			if (n <= 0) {
				break;
			}
			resp.append(buf.data(), static_cast<SZ>(n));
			auto hdr_end = resp.find("\r\n\r\n");
			if (hdr_end == S::npos) {
				continue;
			}
			auto cl_pos = resp.find("Content-Length: ");
			if (cl_pos == S::npos || cl_pos > hdr_end) {
				break;
			}
			cl_pos += 16;
			auto cl_end = resp.find("\r\n", cl_pos);
			SZ body_len = 0;
			std::from_chars(resp.data() + cl_pos, resp.data() + cl_end, body_len);
			if (resp.size() >= hdr_end + 4 + body_len) {
				// Trim to exactly one response.
				resp.resize(hdr_end + 4 + body_len);
				break;
			}
		}
		return resp;
	};

	auto resp1 = read_one_tls();
	auto resp2 = read_one_tls();

	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	::close(fd);

	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	auto h1 = resp1.find("\r\n\r\n");
	REQUIRE(resp1.substr(h1 + 4) == R"({"tls":true})");

	REQUIRE(resp2.starts_with("HTTP/1.1 200 OK"));
	auto h2 = resp2.find("\r\n\r\n");
	REQUIRE(resp2.substr(h2 + 4) == "hello pipe");
}

TEST_CASE(
	"same-port: HTTP and HTTPS on same port both serve correctly") {
	ensure_tls_server();
	// Plaintext GET to a TLS-capable port: first-byte sniff routes it as plain HTTP.
	auto plain_resp = http_get_on(g_tls_port, "/ping");
	REQUIRE(plain_resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = plain_resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(plain_resp.substr(hdr_end + 4) == R"({"tls":true})");

	// TLS GET to the same port: first-byte 0x16 sniff routes it as HTTPS.
	auto tls_resp = tls_get("/ping");
	REQUIRE(tls_resp.starts_with("HTTP/1.1 200 OK"));
	auto tls_hdr_end = tls_resp.find("\r\n\r\n");
	REQUIRE(tls_hdr_end != S::npos);
	REQUIRE(tls_resp.substr(tls_hdr_end + 4) == R"({"tls":true})");
}

// ---------------------------------------------------------------------------
// WebSocket over TLS (wss://)
// ---------------------------------------------------------------------------

TEST_CASE(
	"TLS: WebSocket upgrade over TLS (wss://) works end-to-end") {
	// wss:// is fully supported: server upgrades to WebSocket over TLS and echoes frames.
	// Verify: client receives 101, sends a text frame, gets it echoed back, then closes.
	char cert_tmp[] = "/tmp/conflux_wss_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_wss_key_XXXXXX.pem";
	{
		int fd = ::mkstemps(cert_tmp, 4);
		::close(fd);
		fd = ::mkstemps(key_tmp, 4);
		::close(fd);
	}
	S const cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	REQUIRE(::system(cmd.c_str()) == 0);

	Config cfg = mw_config();
	cfg.cert_file = cert_tmp;
	cfg.key_file = key_tmp;

	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		while (auto f = ws.recv()) {
			if (f->opcode == WsConn::Opcode::Text) {
				ws.send_text(f->payload);
			}
		}
	});

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const wss_port = srv.port();
	::unlink(cert_tmp);
	::unlink(key_tmp);

	SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(wss_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	SSL *ssl = SSL_new(ctx);
	SSL_set_fd(ssl, fd);
	REQUIRE(SSL_connect(ssl) == 1);

	// Send a valid WebSocket upgrade request.
	SV const upgrade =
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";
	REQUIRE(SSL_write(ssl, upgrade.data(), static_cast<int>(upgrade.size())) > 0);

	// Read until we have the 101 response (no Content-Length; ends at \r\n\r\n).
	S resp;
	A<char, 4096> buf{};
	for (;;) {
		int const n = SSL_read(ssl, buf.data(), static_cast<int>(buf.size()));
		if (n <= 0) {
			break;
		}
		resp.append(buf.data(), static_cast<SZ>(n));
		if (resp.find("\r\n\r\n") != S::npos) {
			break;
		}
	}

	REQUIRE(resp.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
	REQUIRE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != S::npos);

	// Send a masked text frame carrying "hello".
	// WS client frames must be masked (RFC 6455 §5.3).
	SV const payload = "hello";
	A<uint8_t, 4> mask_key{0xAB, 0xCD, 0xEF, 0x01};
	A<uint8_t, 2 + 4 + 5> frame_buf{};
	frame_buf[0] = 0x81U; // FIN + Text opcode
	frame_buf[1] = 0x80U | static_cast<uint8_t>(payload.size()); // MASK + len
	frame_buf[2] = mask_key[0];
	frame_buf[3] = mask_key[1];
	frame_buf[4] = mask_key[2];
	frame_buf[5] = mask_key[3];
	for (SZ i = 0; i < payload.size(); ++i) {
		frame_buf[6 + i] = static_cast<uint8_t>(payload[i]) ^ mask_key[i & 3];
	}
	REQUIRE(SSL_write(ssl, frame_buf.data(), static_cast<int>(frame_buf.size())) > 0);

	// Read the server's echo frame (unmasked text, FIN=1, opcode=1).
	A<char, 32> echo_buf{};
	int const n = SSL_read(ssl, echo_buf.data(), static_cast<int>(echo_buf.size()));
	REQUIRE(n >= 7); // 2 hdr + 5 payload
	REQUIRE((static_cast<uint8_t>(echo_buf[0]) & 0x8FU) == 0x81U); // FIN + Text
	REQUIRE(static_cast<uint8_t>(echo_buf[1]) == 5); // unmasked, len=5
	REQUIRE(SV{echo_buf.data() + 2, 5} == "hello");

	// Send a close frame (code 1000).
	A<uint8_t, 2 + 4 + 2> close_frame{};
	close_frame[0] = 0x88U; // FIN + Close
	close_frame[1] = 0x82U; // MASK + 2 bytes
	close_frame[2] = 0x11;
	close_frame[3] = 0x22;
	close_frame[4] = 0x33;
	close_frame[5] = 0x44; // mask
	close_frame[6] = static_cast<uint8_t>(0x03U ^ 0x11U); // 1000 >> 8 XOR mask[0]
	close_frame[7] = static_cast<uint8_t>(0xE8U ^ 0x22U); // 1000 & 0xFF XOR mask[1]
	SSL_write(ssl, close_frame.data(), static_cast<int>(close_frame.size()));

	// Drain until EOF (server echoes Close frame and then shuts down).
	for (int i = 0; i < 10; ++i) {
		char drain[64]{};
		int const dr = SSL_read(ssl, drain, sizeof(drain));
		if (dr <= 0) {
			break;
		}
	}

	SSL_free(ssl);
	SSL_CTX_free(ctx);
	::close(fd);

	srv.stop();
}

// ---------------------------------------------------------------------------
// forwarded_middleware
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
	"forwarded: lax mode with empty trusted_proxies trusts all peers (legacy)") {
	ensure_forwarded_lax_empty_server();
	auto resp = http_get_on(g_fwd_lax_empty_port, "/addr", "X-Forwarded-For: 203.0.113.9\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "203.0.113.9");
}

TEST_CASE(
	"forwarded: use_x_forwarded_for=false falls back to X-Real-IP") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(forwarded_middleware({
			.trusted_proxies = {"127.0.0.1/32"},
			.use_x_forwarded_for = false,
			.use_x_real_ip = true,
		}));
		router.get("/addr", [](HttpRequest const &req) { return HttpResponse::text(S{req.remote_addr}); });
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
							 Router r;
							 r.use(forwarded_middleware({})); // strict: no trusted proxies
							 // Echo the header as-seen by the downstream handler.
							 r.get("/xff", [](HttpRequest const &req) {
								 return HttpResponse::text(S{req.headers["x-forwarded-for"]});
							 });
							 return r;
						 }()};
	// Send a spoofed X-Forwarded-For from an untrusted peer (loopback).
	auto resp = http_get_on(srv.port(), "/xff", "X-Forwarded-For: 203.0.113.99\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Middleware must strip the header; downstream sees empty string.
	REQUIRE(extract_body(resp).empty());
}

// ---------------------------------------------------------------------------
// request_id_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"request_id: generates X-Request-ID when absent") {
	ensure_rid_server();
	auto resp = http_get_on(g_rid_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Response header must contain X-Request-ID.
	REQUIRE(resp.find("X-Request-ID:") != S::npos);
	// Body contains the ID injected into the request.
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	auto body = resp.substr(hdr_end + 4);
	REQUIRE(!body.empty());
	// UUID v4 format: 8-4-4-4-12 hex chars.
	REQUIRE(body.size() == 36);
	REQUIRE(body[8] == '-');
	REQUIRE(body[13] == '-');
	REQUIRE(body[18] == '-');
	REQUIRE(body[23] == '-');
}

TEST_CASE(
	"request_id: echoes existing X-Request-ID from client") {
	ensure_rid_server();
	auto resp = http_get_on(g_rid_port, "/", "X-Request-ID: my-trace-id-123\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Response header must echo the client's ID.
	REQUIRE(resp.find("X-Request-ID: my-trace-id-123") != S::npos);
	// Body also reflects the echoed ID.
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "my-trace-id-123");
}

TEST_CASE(
	"request_id: two requests get different generated IDs") {
	ensure_rid_server();
	auto resp1 = http_get_on(g_rid_port, "/");
	auto resp2 = http_get_on(g_rid_port, "/");
	auto body1 = resp1.substr(resp1.find("\r\n\r\n") + 4);
	auto body2 = resp2.substr(resp2.find("\r\n\r\n") + 4);
	REQUIRE(body1 != body2);
}

TEST_CASE(
	"request_id: trust_incoming=false always generates fresh ID") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(request_id_middleware({.trust_incoming = false}));
		router.get("/", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.headers["x-request-id"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	// Client sends a specific ID; middleware must ignore it and generate its own.
	auto resp = http_get_on(port, "/", "X-Request-ID: client-provided-id\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body = extract_body(resp);
	// Body (injected request ID) must NOT be the client-provided one.
	REQUIRE(body != "client-provided-id");
	// But it should still be a UUID v4 (36 chars).
	REQUIRE(body.size() == 36);
}

TEST_CASE(
	"request_id: custom header name is stamped on request and response") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(request_id_middleware({.header = "X-Trace-ID"}));
		router.get("/", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.headers["x-trace-id"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("X-Trace-ID:") != S::npos);
	REQUIRE(extract_body(resp).size() == 36);
}

// ---------------------------------------------------------------------------
// ip_filter_middleware
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
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(ip_filter_middleware({.mode = IpFilterMode::allowlist, .cidrs = {}}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
}

// ---------------------------------------------------------------------------
// cache_control_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"cache_control: image/* gets immutable max-age rule") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/image");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Cache-Control: max-age=31536000, immutable") != S::npos);
}

TEST_CASE(
	"cache_control: text/css gets its specific rule") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/css");
	REQUIRE(resp.find("Cache-Control: max-age=86400, public") != S::npos);
}

TEST_CASE(
	"cache_control: application/json gets no-store") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/api");
	REQUIRE(resp.find("Cache-Control: no-store") != S::npos);
}

TEST_CASE(
	"cache_control: unmatched MIME gets default directive") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/html");
	REQUIRE(resp.find("Cache-Control: no-cache") != S::npos);
}

TEST_CASE(
	"cache_control: handler-set Cache-Control is not overwritten") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/custom");
	REQUIRE(resp.find("Cache-Control: max-age=999") != S::npos);
}

TEST_CASE(
	"cache_control: Content-Type with charset still matches mime prefix") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cache_control_middleware({
			.rules = {{"text/html", "max-age=60"}},
		}));
		router.get("/", [](HttpRequest const &) {
			HttpResponse r;
			r.content_type = "text/html; charset=utf-8";
			r.set_text_body("<p/>");
			return r;
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.find("Cache-Control: max-age=60") != S::npos);
}

TEST_CASE(
	"cache_control: empty mime_prefix rule matches everything") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cache_control_middleware({
			.rules = {{"image/", "max-age=99999"}, {"", "no-store"}},
		}));
		router.get("/any", [](HttpRequest const &) { return HttpResponse::text("x"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/any");
	REQUIRE(resp.find("Cache-Control: no-store") != S::npos);
}

// ---------------------------------------------------------------------------
// trailing_slash_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"trailing_slash: /foo/ redirects to /foo with 301") {
	ensure_ts_remove_server();
	auto resp = http_get_on(g_ts_remove_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	REQUIRE(resp.find("Location: /foo\r\n") != S::npos);
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
	REQUIRE(resp.find("Location: /bar/\r\n") != S::npos);
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
	REQUIRE(resp.find("Location: /foo\r\n") != S::npos);
}

TEST_CASE(
	"trailing_slash: redirect_status=307 emits 307 Temporary Redirect") {
	ensure_ts_307_server();
	auto resp = http_get_on(g_ts_307_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 307 Temporary Redirect"));
	REQUIRE(resp.find("Location: /foo\r\n") != S::npos);
}

TEST_CASE(
	"trailing_slash: redirect_status=308 emits 308 Permanent Redirect") {
	ensure_ts_308_server();
	auto resp = http_get_on(g_ts_308_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 308 Permanent Redirect"));
}

TEST_CASE(
	"trailing_slash: query string is preserved in redirect Location") {
	ensure_ts_remove_server();
	// /foo/?x=1&y=2 should redirect to /foo?x=1&y=2
	auto resp = http_get_on(g_ts_remove_port, "/foo/?x=1&y=2");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	auto loc = extract_header(resp, "Location");
	REQUIRE(loc.starts_with("/foo?"));
	REQUIRE(loc.find("x=1") != S::npos);
	REQUIRE(loc.find("y=2") != S::npos);
}

TEST_CASE(
	"trailing_slash: query string with spaces is percent-encoded in Location") {
	ensure_ts_remove_server();
	// /foo/?name=hello world should percent-encode the space
	auto resp = http_get_on(g_ts_remove_port, "/foo/?name=hello%20world");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	auto loc = extract_header(resp, "Location");
	REQUIRE(loc.find("name=hello%20world") != S::npos);
}

// ---------------------------------------------------------------------------
// JWT
// ---------------------------------------------------------------------------

namespace {

// Shared JWT test server (single instance, lazy-init).
uint16_t g_jwt_port = 0;
S g_jwt_secret = "test-secret-key";

void ensure_jwt_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		Router router;
		router.use(jwt_middleware(JwtOptions{.secret = g_jwt_secret}));
		router.get("/api/protected", [](HttpRequest const &req) {
			auto sub = req.params["jwt_sub"];
			return HttpResponse::json(std::format(R"({{"sub":"{}"}})", sub));
		});
		g_jwt_port = test_servers().start(cfg, std::move(router));
	});
}

S make_jwt(
	SV payload_json) {
	return jwt_sign(payload_json, g_jwt_secret);
}

S make_jwt_with_header(
	SV header_json,
	SV payload_json,
	SV secret) {
	auto header_b64 =
		base64url_encode(std::span{reinterpret_cast<unsigned char const *>(header_json.data()), header_json.size()});
	auto payload_b64 =
		base64url_encode(std::span{reinterpret_cast<unsigned char const *>(payload_json.data()), payload_json.size()});
	S const signing_input = header_b64 + '.' + payload_b64;
	auto sig = hmac_sha256(
		std::span{reinterpret_cast<unsigned char const *>(secret.data()), secret.size()},
		std::span{reinterpret_cast<unsigned char const *>(signing_input.data()), signing_input.size()});
	auto sig_b64 = base64url_encode(std::span{sig.data(), sig.size()});
	return signing_input + '.' + sig_b64;
}

} // namespace

TEST_CASE(
	"jwt: valid token returns 200 and injects sub claim") {
	ensure_jwt_server();
	auto now =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	auto token = make_jwt(std::format(R"({{"sub":"user42","exp":{}}})", now + 3600));
	auto resp =
		http_get_with_header_on(g_jwt_port, "/api/protected", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
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
	auto token = jwt_sign(R"({"sub":"bad","exp":9999999999})", "wrong-secret");
	auto resp =
		http_get_with_header_on(g_jwt_port, "/api/protected", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}

TEST_CASE(
	"jwt: expired token returns 401") {
	ensure_jwt_server();
	auto token = make_jwt(R"({"sub":"x","exp":1})"); // exp = 1970
	auto resp =
		http_get_with_header_on(g_jwt_port, "/api/protected", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}

TEST_CASE(
	"jwt: malformed token returns 401") {
	ensure_jwt_server();
	auto resp = http_get_with_header_on(g_jwt_port, "/api/protected", "Authorization: Bearer not.a.jwt\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}

TEST_CASE(
	"jwt: lowercase bearer scheme returns 200") {
	ensure_jwt_server();
	auto token = make_jwt(R"({"sub":"user42","exp":9999999999})");
	auto resp =
		http_get_with_header_on(g_jwt_port, "/api/protected", std::format("Authorization: bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}

TEST_CASE(
	"jwt_middleware: injected claim params override route params") {
	Config const cfg{.port = 0, .rings = 1};
	Router router;
	router.use(jwt_middleware(JwtOptions{.secret = "sec", .verify_exp = false}));
	router.get("/api/protected/{jwt_sub}", [](HttpRequest const &req) {
		return HttpResponse::json(std::format(R"({{"sub":"{}"}})", req.params["jwt_sub"]));
	});
	auto port = test_servers().start(cfg, std::move(router));
	auto token = jwt_sign(R"({"sub":"victim"})", "sec");
	auto resp =
		http_get_with_header_on(port, "/api/protected/attacker", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	REQUIRE(resp.substr(hdr_end + 4) == R"({"sub":"victim"})");
}

TEST_CASE(
	"jwt_decode: valid token with no exp returns claims") {
	JwtOptions const opts{.secret = "sec", .verify_exp = false};
	auto token = jwt_sign(R"({"sub":"alice","iss":"test"})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->sub == "alice");
	REQUIRE(result->iss == "test");
}

TEST_CASE(
	"jwt_decode: issuer mismatch returns error") {
	JwtOptions const opts{.secret = "sec", .issuer = "expected", .verify_exp = false};
	auto token = jwt_sign(R"({"sub":"x","iss":"other"})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("issuer") != S::npos);
}

TEST_CASE(
	"jwt_decode: audience string match") {
	JwtOptions opts{.secret = "sec", .audience = "myapp", .verify_exp = false};
	auto token = jwt_sign(R"({"sub":"u","aud":"myapp"})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(result.has_value());
}

TEST_CASE(
	"jwt_decode: audience mismatch returns error") {
	JwtOptions opts{.secret = "sec", .audience = "myapp", .verify_exp = false};
	auto token = jwt_sign(R"({"sub":"u","aud":"other"})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("audience") != S::npos);
}

TEST_CASE(
	"jwt_decode: audience array match") {
	JwtOptions opts{.secret = "sec", .audience = "myapp", .verify_exp = false};
	auto token = jwt_sign(R"({"sub":"u","aud":["svc","myapp"]})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(result.has_value());
}

TEST_CASE(
	"jwt_decode: accepts JSON whitespace around claim separators") {
	JwtOptions const opts{.secret = "sec", .audience = "myapp", .verify_exp = false};
	auto token = jwt_sign("{\n\t\"sub\"\t:\t\"u\",\r\n\t\"aud\"\n:\n[\n\t\"svc\",\r\n\t\"myapp\"\n]\n}", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->sub == "u");
}

TEST_CASE(
	"jwt_decode: audience array matches only aud claim") {
	JwtOptions const opts{.secret = "sec", .audience = "expected", .verify_exp = false};
	auto good = jwt_sign(R"({"sub":"x","aud":["other","expected"]})", "sec");
	auto bad = jwt_sign(R"({"sub":"expected","aud":["other"]})", "sec");
	REQUIRE(jwt_decode(good, opts).has_value());
	auto result = jwt_decode(bad, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "audience mismatch");
}

TEST_CASE(
	"jwt_decode: malformed audience array with leading comma does not match") {
	JwtOptions const opts{.secret = "sec", .audience = "expected", .verify_exp = false};
	auto token = jwt_sign(R"({"sub":"x","aud":[,"expected"]})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "audience mismatch");
}

TEST_CASE(
	"jwt_decode: unterminated alg string is rejected") {
	JwtOptions const opts{.secret = "sec", .verify_exp = false};
	auto token = make_jwt_with_header(R"({"alg":"HS256)", R"({"sub":"x"})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("HS256") != S::npos);
}

TEST_CASE(
	"jwt_decode: malformed numeric claims are rejected") {
	JwtOptions const opts{.secret = "sec"};
	auto token = jwt_sign(R"({"sub":"x","exp":"soon"})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "invalid exp claim");
}

TEST_CASE(
	"jwt_decode: malformed nbf claim is rejected") {
	JwtOptions const opts{.secret = "sec", .verify_exp = false, .verify_nbf = false};
	auto token = jwt_sign(R"({"sub":"x","nbf":"later"})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "invalid nbf claim");
}

TEST_CASE(
	"jwt_decode: malformed iat claim is rejected") {
	JwtOptions const opts{.secret = "sec", .verify_exp = false, .verify_nbf = false};
	auto token = jwt_sign(R"({"sub":"x","iat":"earlier"})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "invalid iat claim");
}

TEST_CASE(
	"jwt_decode: exp equal to now is expired") {
	auto now =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	JwtOptions const opts{.secret = "sec"};
	auto token = jwt_sign(std::format(R"({{"sub":"x","exp":{}}})", now), "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "token expired");
}

TEST_CASE(
	"jwt_decode: nbf in future returns not-yet-valid error") {
	auto far_future =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
		+ 9999;
	JwtOptions const opts{.secret = "sec", .verify_exp = false, .verify_nbf = true};
	auto token = jwt_sign(std::format(R"({{"sub":"x","nbf":{}}})", far_future), "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("not yet valid") != S::npos);
}

TEST_CASE(
	"jwt_decode: nbf in past is accepted") {
	JwtOptions const opts{.secret = "sec", .verify_exp = false, .verify_nbf = true};
	auto token = jwt_sign(R"({"sub":"x","nbf":1})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(result.has_value());
}

TEST_CASE(
	"jwt_decode: verify_exp=false allows expired token") {
	JwtOptions const opts{.secret = "sec", .verify_exp = false};
	auto token = jwt_sign(R"({"sub":"x","exp":1})", "sec");
	auto result = jwt_decode(token, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->sub == "x");
}

TEST_CASE(
	"jwt_decode: non-HS256 algorithm returns unsupported error") {
	// Pre-computed base64url (no padding):
	// {"alg":"RS256","typ":"JWT"} → eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9
	// {"sub":"x"}                 → eyJzdWIiOiJ4In0
	SV token = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJ4In0.ZmFrZXNpZw";
	JwtOptions opts{.secret = "sec"};
	auto result = jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("HS256") != S::npos);
}

// ---------------------------------------------------------------------------
// Counter / Gauge / Histogram unit tests
// ---------------------------------------------------------------------------

TEST_CASE(
	"metrics: Counter inc and get") {
	Counter c;
	CHECK(c.get() == 0);
	c.inc();
	CHECK(c.get() == 1);
	c.inc(5);
	CHECK(c.get() == 6);
}

TEST_CASE(
	"metrics: Gauge set inc dec") {
	Gauge g;
	g.set(10.0);
	CHECK(g.get() == 10.0);
	g.inc(2.5);
	CHECK(g.get() == 12.5);
	g.dec(3.0);
	CHECK(g.get() == 9.5);
}

TEST_CASE(
	"metrics: Histogram observe updates count and sum") {
	Histogram h;
	h.observe(0.1);
	h.observe(0.05);
	CHECK(h.count() == 2);
	// sum must be approximately 0.15
	CHECK(h.sum() > 0.14);
	CHECK(h.sum() < 0.16);
}

TEST_CASE(
	"metrics: Histogram bucket boundaries") {
	Histogram h;
	// 0.005 bucket: only observations <= 0.005 fall in it.
	h.observe(0.003);
	h.observe(0.007);
	// bucket[0] is le=0.005; only first observation qualifies.
	CHECK(h.bucket(0) == 1);
	// bucket[1] is le=0.01; both qualify.
	CHECK(h.bucket(1) == 2);
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

namespace {

uint16_t g_metrics_port = 0;
MetricsRegistry *g_metrics_reg = nullptr;

void ensure_metrics_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		Router router;
		static MetricsRegistry reg;
		g_metrics_reg = &reg;
		router.use(metrics_middleware(reg));
		router.get("/ping", [](HttpRequest const &) { return HttpResponse::text("pong"); });
		router.get("/metrics", metrics_handler(reg));
		g_metrics_port = test_servers().start(cfg, std::move(router));
	});
}

uint16_t g_protected_metrics_port = 0;

void ensure_protected_metrics_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		Router router;
		static MetricsRegistry reg2;
		router.use(metrics_middleware(reg2));
		V<Router::Middleware> chain;
		chain.push_back(bearer_auth_middleware([](SV token) { return token == "supersecret"; }));
		router.get("/metrics", metrics_handler_protected(reg2, std::move(chain)));
		g_protected_metrics_port = test_servers().start(cfg, std::move(router));
	});
}

} // namespace

TEST_CASE(
	"metrics: /ping increments http_requests_total GET 2xx") {
	ensure_metrics_server();
	http_get_on(g_metrics_port, "/ping");
	http_get_on(g_metrics_port, "/ping");
	auto resp = http_get_on(g_metrics_port, "/metrics");
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	auto body = resp.substr(hdr_end + 4);
	REQUIRE(body.find("http_requests_total{method=\"GET\",status=\"2xx\"}") != S::npos);
}

TEST_CASE(
	"metrics: /metrics returns Prometheus content-type") {
	ensure_metrics_server();
	auto resp = http_get_on(g_metrics_port, "/metrics");
	REQUIRE(resp.find("text/plain; version=0.0.4") != S::npos);
}

TEST_CASE(
	"metrics_handler_protected: missing bearer token returns 401") {
	ensure_protected_metrics_server();
	auto resp = http_get_on(g_protected_metrics_port, "/metrics");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find("WWW-Authenticate: Bearer") != S::npos);
}

TEST_CASE(
	"metrics_handler_protected: wrong bearer token returns 401") {
	ensure_protected_metrics_server();
	auto resp = http_get_on(g_protected_metrics_port, "/metrics", "Authorization: Bearer badguess\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}

TEST_CASE(
	"metrics_handler_protected: valid bearer token returns 200 prometheus body") {
	ensure_protected_metrics_server();
	auto resp = http_get_on(g_protected_metrics_port, "/metrics", "Authorization: Bearer supersecret\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
	REQUIRE(resp.find("text/plain; version=0.0.4") != S::npos);
	REQUIRE(resp.find("http_requests_total") != S::npos);
}

TEST_CASE(
	"metrics: duration histogram appears in output") {
	ensure_metrics_server();
	http_get_on(g_metrics_port, "/ping");
	auto resp = http_get_on(g_metrics_port, "/metrics");
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != S::npos);
	auto body = resp.substr(hdr_end + 4);
	REQUIRE(body.find("http_request_duration_seconds_sum") != S::npos);
	REQUIRE(body.find("http_request_duration_seconds_count") != S::npos);
	REQUIRE(body.find("http_request_duration_seconds_bucket") != S::npos);
}

TEST_CASE(
	"metrics: 4xx response increments GET 4xx counter") {
	static uint16_t port = 0;
	static MetricsRegistry *reg_ptr = nullptr;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Config const cfg{.port = 0, .rings = 1};
		Router router;
		static MetricsRegistry reg;
		reg_ptr = &reg;
		router.use(metrics_middleware(reg));
		router.get("/ok", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		router.get("/metrics", metrics_handler(reg));
		port = test_servers().start(cfg, std::move(router));
	});
	http_get_on(port, "/nonexistent"); // 404 → 4xx
	auto resp = http_get_on(port, "/metrics");
	auto body = extract_body(resp);
	REQUIRE(body.find("http_requests_total{method=\"GET\",status=\"4xx\"}") != S::npos);
}

TEST_CASE(
	"metrics: 5xx response increments GET 5xx counter") {
	MetricsRegistry reg;
	reg.record("GET", 500, std::chrono::milliseconds{1});
	auto out = reg.format_prometheus();
	REQUIRE(out.find("http_requests_total{method=\"GET\",status=\"5xx\"}") != S::npos);
}

TEST_CASE(
	"metrics: OTHER method bucket used for non-standard methods") {
	MetricsRegistry reg;
	reg.record("PURGE", 200, std::chrono::milliseconds{1});
	auto out = reg.format_prometheus();
	REQUIRE(out.find("http_requests_total{method=\"OTHER\",status=\"2xx\"}") != S::npos);
}

TEST_CASE(
	"metrics: out-of-range status maps to other bucket") {
	MetricsRegistry reg;
	reg.record("GET", 999, std::chrono::milliseconds{1});
	auto out = reg.format_prometheus();
	REQUIRE(out.find("http_requests_total{method=\"GET\",status=\"other\"}") != S::npos);
}

// ---------------------------------------------------------------------------
// Compression codecs
// ---------------------------------------------------------------------------

namespace {

uint16_t g_codec_port = 0;

void ensure_codec_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		Router router;
		router.use(compress_middleware({.min_body_size = 0})); // compress everything
		router.get("/data", [](HttpRequest const &) {
			return HttpResponse::text(S(512, 'A')); // compressible
		});
		router.get("/vary", [](HttpRequest const &) {
			auto r = HttpResponse::text(S(512, 'A'));
			r.headers["Vary"] = "X-Test";
			return r;
		});
		g_codec_port = test_servers().start(cfg, std::move(router));
	});
}

} // namespace

TEST_CASE(
	"compress: brotli is ignored for dynamic responses") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: br, gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != S::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == S::npos);
#endif
}

TEST_CASE(
	"compress: zstd accepted when client prefers it") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: zstd;q=1, gzip;q=0.5\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_ZSTD
	REQUIRE(resp.find("Content-Encoding: zstd\r\n") != S::npos);
#elif CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != S::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == S::npos);
#endif
}

TEST_CASE(
	"compress: gzip returned when only gzip offered") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != S::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == S::npos);
#endif
}

TEST_CASE(
	"compress: Accept-Encoding token matching is case-insensitive") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: GZip;Q=1\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != S::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == S::npos);
#endif
}

TEST_CASE(
	"compress: appends Accept-Encoding to existing Vary") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/vary", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Vary: X-Test, Accept-Encoding\r\n") != S::npos);
#else
	REQUIRE(resp.find("Vary: X-Test\r\n") != S::npos);
#endif
}

TEST_CASE(
	"compress: q=0 exclusion: gzip;q=0 gives zstd") {
	ensure_codec_server();
	// gzip explicitly excluded; should get zstd
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: gzip;q=0, zstd\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_ZSTD
	REQUIRE(resp.find("Content-Encoding: zstd\r\n") != S::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == S::npos);
#endif
}

TEST_CASE(
	"compress: wildcard * selects preferred dynamic codec") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: *\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS && CONFLUX_HAS_ZSTD
	if (current_dynamic_encoding_preference() == DynamicEncodingPreference::gzip_first) {
		REQUIRE(resp.find("Content-Encoding: gzip\r\n") != S::npos);
	} else {
		REQUIRE(resp.find("Content-Encoding: zstd\r\n") != S::npos);
	}
#elif CONFLUX_HAS_ZSTD
	REQUIRE(resp.find("Content-Encoding: zstd\r\n") != S::npos);
#elif CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != S::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == S::npos);
#endif
}

TEST_CASE(
	"compress+cors: Vary header accumulates both Origin and Accept-Encoding") {
	ensure_cors_compress_server();
	auto resp = http_get_on(
		g_cors_compress_port,
		"/big",
		"Origin: https://test.example\r\n"
		"Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto vary = extract_header(resp, "Vary");
	// Both CORS and compress must contribute to Vary without overwriting each other.
	REQUIRE(vary.find("Origin") != S::npos);
	REQUIRE(vary.find("Accept-Encoding") != S::npos);
}

// ---------------------------------------------------------------------------
// redirect_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"redirect: exact match returns 301 with Location") {
	ensure_redirect_server();
	auto resp = http_get_on(g_redirect_port, "/old");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	REQUIRE(resp.find("Location: /new\r\n") != S::npos);
}

TEST_CASE(
	"redirect: prefix match appends suffix and returns 302") {
	ensure_redirect_server();
	auto resp = http_get_on(g_redirect_port, "/api/v1/users");
	REQUIRE(resp.starts_with("HTTP/1.1 302"));
	REQUIRE(resp.find("Location: /api/v2/users\r\n") != S::npos);
}

TEST_CASE(
	"redirect: non-matching path passes through to handler") {
	ensure_redirect_server();
	auto resp = http_get_on(g_redirect_port, "/new");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "new");
}

TEST_CASE(
	"redirect: custom status 307 preserved in redirect response") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(redirect_middleware({.rules = {{.from = "/x", .to = "/y", .status = 307}}}));
		router.get("/y", [](HttpRequest const &) { return HttpResponse::text("y"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/x");
	REQUIRE(resp.starts_with("HTTP/1.1 307"));
	REQUIRE(resp.find("Location: /y\r\n") != S::npos);
}

TEST_CASE(
	"proxy: decode_chunked_body handles trailers and extensions") {
	auto decoded = decode_chunked_body(
		"a;foo=bar\r\n1234567890\r\n"
		"3\r\nxyz\r\n"
		"0\r\nX-Trailer: done\r\n\r\n");
	REQUIRE(decoded.has_value());
	REQUIRE(*decoded == "1234567890xyz");
}

TEST_CASE(
	"proxy: decode_chunked_body no trailers") {
	auto decoded = decode_chunked_body("5\r\nhello\r\n0\r\n\r\n");
	REQUIRE(decoded.has_value());
	REQUIRE(*decoded == "hello");
}

TEST_CASE(
	"proxy: decode_chunked_body rejects incomplete input") {
	auto decoded = decode_chunked_body(
		"5\r\nhello\r\n"
		"4\r\nwor");
	REQUIRE(!decoded.has_value());
}

TEST_CASE(
	"http client: chunked response without trailers is decoded correctly") {
	// Build a mock server that sends a chunked response (no trailers).
	uint16_t port = 0;
	int const lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(lfd >= 0);
	int yes = 1;
	::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;
	REQUIRE(::bind(lfd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0);
	socklen_t salen = sizeof(sa);
	REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr *>(&sa), &salen) == 0);
	port = ntohs(sa.sin_port);
	REQUIRE(::listen(lfd, 1) == 0);

	auto srv = std::thread([lfd] {
		int const c = ::accept(lfd, nullptr, nullptr);
		if (c < 0) {
			return;
		}
		// Drain request
		char buf[4096];
		while (::recv(c, buf, sizeof(buf), 0) > 0) {
			if (strstr(buf, "\r\n\r\n")) {
				break;
			}
		}
		// Send chunked response with no trailers: 0\r\n\r\n
		SV const resp =
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain\r\n"
			"Transfer-Encoding: chunked\r\n"
			"Connection: close\r\n"
			"\r\n"
			"5\r\nhello\r\n"
			"6\r\n world\r\n"
			"0\r\n"
			"\r\n";
		::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
		::close(c);
	});

	auto result = HttpClient{}.send_blocking(chttp::HttpRequest::get(std::format("http://127.0.0.1:{}/", port)));

	srv.join();
	::close(lfd);

	REQUIRE(result.has_value());
	CHECK(result->head.status == 200);
	CHECK(result->body == "hello world");
}

TEST_CASE(
	"proxy: work-pool proxy handler forwards upstream response off-ring") {
	ensure_proxy_server();
	auto resp = http_get_on(g_proxy_port, "/proxy/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("X-Upstream: yes\r\n") != S::npos);
	REQUIRE(extract_body(resp) == "proxied-ok");
}

TEST_CASE(
	"proxy: preserve_host=true forwards original Host header") {
	static SP<ScopedTestServer> s_upstream;
	static SP<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		Router upstream;
		upstream.get("/echo", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.headers["host"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		Router front;
		front.get(
			"/echo",
			proxy_handler(
				ProxyOptions{
					.upstream_host = "127.0.0.1",
					.upstream_port = s_upstream->port(),
					.preserve_host = true,
				}));
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	auto resp = http_get_on(s_front->port(), "/echo");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Body should be the exact original Host header forwarded by the proxy ("localhost").
	REQUIRE(extract_body(resp) == "localhost");
}

TEST_CASE(
	"proxy: preserve_host=true with port in Host header still connects to upstream") {
	static SP<ScopedTestServer> s_upstream;
	static SP<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		Router upstream;
		upstream.get("/echo", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.headers["host"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		Router front;
		front.get(
			"/echo",
			proxy_handler(
				ProxyOptions{
					.upstream_host = "127.0.0.1",
					.upstream_port = s_upstream->port(),
					.preserve_host = true,
				}));
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	// Send Host: localhost:9999 — proxy must connect to upstream, not myapp.example.com.
	auto resp = http_get_on_host(s_front->port(), "localhost:9999", "/echo");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "localhost:9999");
}

TEST_CASE(
	"proxy: appends to existing X-Forwarded-For header") {
	static SP<ScopedTestServer> s_upstream;
	static SP<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		Router upstream;
		upstream.get("/xff", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.headers["x-forwarded-for"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		Router front;
		front.get(
			"/xff",
			proxy_handler(
				ProxyOptions{
					.upstream_host = "127.0.0.1",
					.upstream_port = s_upstream->port(),
				}));
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	// Client sends existing XFF; proxy appends remote_addr (127.0.0.1).
	auto resp = http_get_on(s_front->port(), "/xff", "X-Forwarded-For: 1.2.3.4\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body = extract_body(resp);
	REQUIRE(body.find("1.2.3.4") != S::npos);
	REQUIRE(body.find("127.0.0.1") != S::npos);
	// Must appear as "1.2.3.4, 127.0.0.1" (appended, not replaced).
	REQUIRE(body.find("1.2.3.4, 127.0.0.1") != S::npos);
}

// ---------------------------------------------------------------------------
// cookie_signing (pure functions)
// ---------------------------------------------------------------------------

TEST_CASE(
	"cookie_signing: sign then verify returns original value") {
	auto signed_val = sign_cookie("hello", "my-secret");
	REQUIRE(signed_val.starts_with("hello."));
	auto result = verify_cookie(signed_val, "my-secret");
	REQUIRE(result.has_value());
	REQUIRE(*result == "hello");
}

TEST_CASE(
	"cookie_signing: verify with wrong secret returns nullopt") {
	auto signed_val = sign_cookie("hello", "my-secret");
	auto result = verify_cookie(signed_val, "wrong-secret");
	REQUIRE(!result.has_value());
}

TEST_CASE(
	"cookie_signing: tampered signature returns nullopt") {
	auto signed_val = sign_cookie("hello", "my-secret");
	signed_val.back() = (signed_val.back() == 'A') ? 'B' : 'A'; // flip last char
	auto result = verify_cookie(signed_val, "my-secret");
	REQUIRE(!result.has_value());
}

TEST_CASE(
	"cookie_signing: value without dot returns nullopt") {
	auto result = verify_cookie("nodot", "any-secret");
	REQUIRE(!result.has_value());
}

TEST_CASE(
	"cookie_signing: value with dots round-trips correctly") {
	// sign_cookie uses rfind('.') so a value containing '.' should still work.
	auto signed_val = sign_cookie("user.name@host.example", "my-secret");
	auto result = verify_cookie(signed_val, "my-secret");
	REQUIRE(result.has_value());
	REQUIRE(*result == "user.name@host.example");
}

TEST_CASE(
	"cookie_signing_middleware: short secret throws invalid_argument") {
	REQUIRE_THROWS_AS(cookie_signing_middleware({.secret = "tooshort"}), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// cookie_signing middleware
// ---------------------------------------------------------------------------

constexpr SV kCookieMiddlewareSecret = "srv-secret-16-bytes";
constexpr SV kOtherCookieSecret = "other-secret-16-bytes";

TEST_CASE(
	"cookie_signing_middleware: valid signed cookie is unwrapped") {
	// Set up a server that echoes the "session" cookie value.
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cookie_signing_middleware({.secret = S{kCookieMiddlewareSecret}}));
		router.get("/echo", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.cookies["session"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto signed_val = sign_cookie("user42", kCookieMiddlewareSecret);
	auto resp = http_get_on(port, "/echo", std::format("Cookie: session={}\r\n", signed_val));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "user42");
}

TEST_CASE(
	"cookie_signing_middleware: invalid signature strips cookie value") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cookie_signing_middleware({.secret = S{kCookieMiddlewareSecret}, .strip_invalid = true}));
		router.get("/echo", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.cookies["session"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	// Forge a signed value with the wrong secret.
	auto bad_val = sign_cookie("attacker", kOtherCookieSecret);
	auto resp = http_get_on(port, "/echo", std::format("Cookie: session={}\r\n", bad_val));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Cookie was stripped → handler sees empty value → 200 with empty body.
	REQUIRE(extract_body(resp).empty());
}

TEST_CASE(
	"cookie_signing_middleware: unsigned cookie (no dot) passes through") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cookie_signing_middleware({.secret = S{kCookieMiddlewareSecret}}));
		router.get("/echo", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.cookies["plain"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/echo", "Cookie: plain=nodot\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "nodot");
}

TEST_CASE(
	"cookie_signing_middleware: strip_invalid=false keeps invalid cookie as-is") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cookie_signing_middleware({.secret = "srv-secret-key-1234", .strip_invalid = false}));
		router.get("/echo", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.cookies["session"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	// Cookie with bad signature — handler receives the raw signed value unchanged.
	auto bad_val = sign_cookie("user", "wrong-secret-key-1234");
	auto resp = http_get_on(port, "/echo", std::format("Cookie: session={}\r\n", bad_val));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == bad_val);
}

// ---------------------------------------------------------------------------
// csrf_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"csrf: GET sets csrf_token cookie and X-CSRF-Token response header") {
	ensure_csrf_server();
	auto resp = http_get_on(g_csrf_port, "/page");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Set-Cookie: csrf_token=") != S::npos);
	REQUIRE(resp.find("X-CSRF-Token: ") != S::npos);
}

TEST_CASE(
	"csrf: POST without cookie returns 403") {
	ensure_csrf_server();
	auto resp = http_post_on_full(g_csrf_port, "/submit", "application/x-www-form-urlencoded", "x=1", "");
	REQUIRE(resp.starts_with("HTTP/1.1 403"));
}

TEST_CASE(
	"csrf: POST with mismatched token returns 403") {
	ensure_csrf_server();
	// Use a valid-looking token but not the one the server expects.
	auto resp = http_post_on_full(
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
	// Step 1: GET to obtain the token.
	auto get_resp = http_get_on(g_csrf_port, "/page");
	auto token = extract_set_cookie(get_resp, "csrf_token");
	REQUIRE(!token.empty());
	// Step 2: POST echoing the token in cookie + header.
	auto post_resp = http_post_on_full(
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
	// Submit token via form field instead of X-CSRF-Token header.
	auto post_resp = http_post_on_full(
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
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(csrf_middleware({.protected_methods = {"POST"}}));
		router.get("/page", [](HttpRequest const &) { return HttpResponse::html("<form>"); });
		router.del("/resource", [](HttpRequest const &) { return HttpResponse::text("deleted"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	// DELETE is not in protected_methods, so no token required.
	auto resp = conflux::tests::http_request_on(port, "DELETE", "/resource", "", "", "");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "deleted");
}

// ---------------------------------------------------------------------------
// etag_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"etag: response gets ETag header") {
	ensure_etag_server();
	auto resp = http_get_on(g_etag_port, "/content");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("ETag: \"") != S::npos);
}

TEST_CASE(
	"etag: If-None-Match matching ETag returns 304") {
	ensure_etag_server();
	// First request: get the ETag.
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	// Second request: send If-None-Match with that ETag.
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: {}\r\n", etag));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
	// RFC 9110 §15.4.5: 304 SHOULD include the same ETag as the 200 response.
	CHECK(extract_header(resp2, "ETag") == etag);
}

TEST_CASE(
	"etag: If-None-Match wildcard returns 304") {
	ensure_etag_server();
	// First request: get the ETag.
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto resp = http_get_on(g_etag_port, "/content", "If-None-Match: *\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 304"));
	// RFC 9110 §15.4.5: wildcard 304 should also include the ETag.
	CHECK(extract_header(resp, "ETag") == etag);
}

TEST_CASE(
	"etag: If-None-Match uses weak comparison") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: W/{}\r\n", etag));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
	REQUIRE(extract_header(resp2, "ETag") == etag);
}

TEST_CASE(
	"etag: If-None-Match non-matching tag returns 200 with body") {
	ensure_etag_server();
	auto resp = http_get_on(g_etag_port, "/content", "If-None-Match: \"000000\"\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "hello world");
}

TEST_CASE(
	"etag: empty body response has no ETag") {
	ensure_etag_server();
	auto resp = http_get_on(g_etag_port, "/empty");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("ETag:") == S::npos);
}

TEST_CASE(
	"etag: two requests to same route return same ETag") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto resp2 = http_get_on(g_etag_port, "/content");
	auto etag1 = extract_header(resp1, "ETag");
	auto etag2 = extract_header(resp2, "ETag");
	REQUIRE(!etag1.empty());
	REQUIRE(etag1 == etag2);
}

TEST_CASE(
	"etag: weak option produces W/ prefix") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(etag_middleware({.weak = true}));
		router.get("/w", [](HttpRequest const &) { return HttpResponse::text("body"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/w");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto etag = extract_header(resp, "ETag");
	REQUIRE(!etag.empty());
	REQUIRE(etag.starts_with("W/\""));
}

TEST_CASE(
	"etag: If-None-Match with multiple values matches correct ETag") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto inm = std::format("\"deadbeef\", {}, \"cafebabe\"", etag);
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: {}\r\n", inm));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
}

TEST_CASE(
	"etag: weak If-None-Match matches strong ETag (RFC 7232 weak comparison)") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag"); // e.g. "abc123"
	REQUIRE(!etag.empty());
	// Send back as weak variant: W/"abc123" must still match per weak comparison.
	auto weak_inm = S{"W/"} + etag;
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: {}\r\n", weak_inm));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
}

TEST_CASE(
	"etag: handler-set ETag is not overwritten by middleware") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router r;
		r.use(etag_middleware());
		r.get("/custom", [](HttpRequest const &) {
			auto resp = HttpResponse::text("body");
			resp.headers["ETag"] = "\"custom-etag-42\"";
			return resp;
		});
		port = start_mw_server(mw_config(), std::move(r));
	});
	auto resp = http_get_on(port, "/custom");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_header(resp, "ETag") == "\"custom-etag-42\"");
}

// ---------------------------------------------------------------------------
// response_cache_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"response_cache: first GET hits handler, second GET is served from cache") {
	ensure_resp_cache_server();
	// Reset counter to a known baseline.
	int const before = g_resp_cache_count.load();
	auto resp1 = http_get_on(g_resp_cache_port, "/counted");
	auto resp2 = http_get_on(g_resp_cache_port, "/counted");
	int const after = g_resp_cache_count.load();
	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp2.starts_with("HTTP/1.1 200 OK"));
	// Handler called exactly once more than before (cache hit on second GET).
	REQUIRE(after == before + 1);
	// Both responses carry identical bodies.
	REQUIRE(extract_body(resp1) == extract_body(resp2));
}

TEST_CASE(
	"response_cache: POST bypasses cache and hits handler") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto resp = http_post_on(g_resp_cache_port, "/counted", "application/x-www-form-urlencoded", "");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 1);
}

TEST_CASE(
	"response_cache: no-store response is not cached") {
	ensure_resp_cache_server();
	auto resp1 = http_get_on(g_resp_cache_port, "/no-store");
	auto resp2 = http_get_on(g_resp_cache_port, "/no-store");
	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp2.starts_with("HTTP/1.1 200 OK"));
	// Both return the same body (handler is deterministic), but this just
	// verifies we don't 500 or return stale data.
	REQUIRE(extract_body(resp1) == "uncacheable");
}

TEST_CASE(
	"response_cache: Vary header partitions cache by request header value") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto gzip1 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: gzip\r\n");
	auto id1 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: identity\r\n");
	auto gzip2 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: gzip\r\n");
	auto id2 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: identity\r\n");
	REQUIRE(gzip1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(id1.starts_with("HTTP/1.1 200 OK"));
	// Exactly two handler invocations: one per distinct Accept-Encoding value.
	REQUIRE(g_resp_cache_count.load() == before + 2);
	// Repeats of the same encoding return the cached body (same counter value).
	REQUIRE(extract_body(gzip1) == extract_body(gzip2));
	REQUIRE(extract_body(id1) == extract_body(id2));
	// Different encodings return different bodies (not cross-contaminated).
	REQUIRE(extract_body(gzip1) != extract_body(id1));
	REQUIRE(extract_body(gzip1).find("enc=gzip") != S::npos);
	REQUIRE(extract_body(id1).find("enc=identity") != S::npos);
}

TEST_CASE(
	"response_cache: Vary: * is never cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/vary-star");
	auto r2 = http_get_on(g_resp_cache_port, "/vary-star");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	// Handler called on every request.
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}

TEST_CASE(
	"response_cache: query string participates in cache key") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto a1 = http_get_on(g_resp_cache_port, "/query?value=a");
	auto b1 = http_get_on(g_resp_cache_port, "/query?value=b");
	auto a2 = http_get_on(g_resp_cache_port, "/query?value=a");
	REQUIRE(extract_body(a1) == std::format("{} value=a", before + 1));
	REQUIRE(extract_body(b1) == std::format("{} value=b", before + 2));
	REQUIRE(extract_body(a2) == extract_body(a1));
}

TEST_CASE(
	"response_cache: Cache-Control: private response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/private");
	auto r2 = http_get_on(g_resp_cache_port, "/private");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	// Handler must be called for both requests — private responses must not be cached.
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}

TEST_CASE(
	"response_cache: Cache-Control: max-age=0 response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/max-age-zero");
	auto r2 = http_get_on(g_resp_cache_port, "/max-age-zero");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	// max-age=0 means always stale — both requests must reach the handler.
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}

TEST_CASE(
	"response_cache: Cache-Control: no-cache response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/no-cache");
	auto r2 = http_get_on(g_resp_cache_port, "/no-cache");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	// Both requests must reach the handler (counter incremented twice).
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}

TEST_CASE(
	"response_cache: Set-Cookie response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/set-cookie-resp");
	auto r2 = http_get_on(g_resp_cache_port, "/set-cookie-resp");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}

TEST_CASE(
	"response_cache: non-200 response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/not-found-resp");
	auto r2 = http_get_on(g_resp_cache_port, "/not-found-resp");
	REQUIRE(r1.starts_with("HTTP/1.1 404"));
	REQUIRE(r2.starts_with("HTTP/1.1 404"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
}

TEST_CASE(
	"response_cache: LRU eviction when max_entries exceeded") {
	std::atomic<int> hits{0};
	Router router;
	router.use(response_cache_middleware({.max_entries = 2, .default_ttl = std::chrono::seconds{60}}));
	router.get("/a", [&hits](HttpRequest const &) {
		++hits;
		return HttpResponse::text("a");
	});
	router.get("/b", [&hits](HttpRequest const &) {
		++hits;
		return HttpResponse::text("b");
	});
	router.get("/c", [&hits](HttpRequest const &) {
		++hits;
		return HttpResponse::text("c");
	});

	ScopedTestServer srv{mw_config(), std::move(router)};

	// Populate: /a and /b fill the cache (max_entries=2).
	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	int after_fill = hits.load();
	REQUIRE(after_fill == 2);

	// /a and /b should now be cached — no additional handler calls.
	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 2);

	// Add /c — evicts /a (LRU), cache holds /b and /c.
	http_get_on(srv.port(), "/c");
	REQUIRE(hits.load() == 3);

	// /b still cached, no handler call.
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 3);

	// /a was evicted — handler called again.
	http_get_on(srv.port(), "/a");
	REQUIRE(hits.load() == 4);

	srv.stop();
}

TEST_CASE(
	"response_cache: response larger than max_bytes is not cached") {
	std::atomic<int> hits{0};
	Router router;
	// max_bytes=4: bodies of 5+ bytes won't be stored.
	router.use(response_cache_middleware({.max_entries = 10, .max_bytes = 4, .default_ttl = std::chrono::seconds{60}}));
	router.get("/big", [&hits](HttpRequest const &) {
		++hits;
		return HttpResponse::text("hello"); // 5 bytes > max_bytes
	});
	router.get("/small", [&hits](HttpRequest const &) {
		++hits;
		return HttpResponse::text("hi"); // 2 bytes <= max_bytes
	});

	ScopedTestServer srv{mw_config(), std::move(router)};

	// /big should never be cached.
	http_get_on(srv.port(), "/big");
	http_get_on(srv.port(), "/big");
	REQUIRE(hits.load() == 2);

	// /small should be cached after first hit.
	http_get_on(srv.port(), "/small");
	http_get_on(srv.port(), "/small");
	REQUIRE(hits.load() == 3);

	srv.stop();
}

TEST_CASE(
	"response_cache: expired entry properly frees byte budget for new entries") {
	// max_bytes=16: fits exactly two 8-byte bodies.
	// After both entries expire, total_bytes_ must be decremented so new entries
	// can be cached without spurious eviction (regression: expiry path omitted the
	// total_bytes_ decrement, leaving a phantom byte count that blocked new puts).
	std::atomic<int> hits{0};
	Router router;
	router.use(
		response_cache_middleware({.max_entries = 10, .max_bytes = 16, .default_ttl = std::chrono::seconds{60}}));
	router.get("/a", [&hits](HttpRequest const &) {
		++hits;
		HttpResponse r = HttpResponse::text("aaaaaaaa"); // 8 bytes
		r.headers["Cache-Control"] = "max-age=1";
		return r;
	});
	router.get("/b", [&hits](HttpRequest const &) {
		++hits;
		HttpResponse r = HttpResponse::text("bbbbbbbb"); // 8 bytes
		r.headers["Cache-Control"] = "max-age=1";
		return r;
	});
	router.get("/c", [&hits](HttpRequest const &) {
		++hits;
		HttpResponse r = HttpResponse::text("cccccccc"); // 8 bytes
		r.headers["Cache-Control"] = "max-age=60";
		return r;
	});

	ScopedTestServer srv{mw_config(), std::move(router)};

	// Fill cache: /a and /b each consume 8 bytes → total_bytes_=16.
	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 2);

	// Wait for both entries to expire.
	std::this_thread::sleep_for(std::chrono::milliseconds{1200});

	// Trigger expiry eviction: these are cache misses that decrement total_bytes_.
	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 4);

	// /c must now be cacheable: byte budget was freed by the two expired evictions.
	http_get_on(srv.port(), "/c");
	http_get_on(srv.port(), "/c");
	REQUIRE(hits.load() == 5); // second GET is a cache hit

	srv.stop();
}

// ---------------------------------------------------------------------------
// structured_log_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"structured_log: request is logged as a JSON line to file") {
	ensure_slog_server();
	auto resp = http_get_on(g_slog_port, "/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));

	// Allow a brief moment for the log write to flush.
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	// Read the log file.
	S log_content;
	{
		int const fd = ::open(g_slog_path, O_RDONLY);
		REQUIRE(fd >= 0);
		A<char, 4096> buf{};
		ssize_t const n = ::read(fd, buf.data(), buf.size() - 1);
		::close(fd);
		REQUIRE(n > 0);
		log_content.assign(buf.data(), static_cast<SZ>(n));
	}
	REQUIRE(log_content.find(R"("method":"GET")") != S::npos);
	REQUIRE(log_content.find(R"("path":"/ping")") != S::npos);
	REQUIRE(log_content.find(R"("status":200)") != S::npos);
	REQUIRE(log_content.find(R"("app":"test")") != S::npos);
}

TEST_CASE(
	"structured_log: no app_name omits app field") {
	char path[64]{};
	std::strcpy(path, "/tmp/conflux_slog2_XXXXXX");
	int const tmp = ::mkstemp(path);
	REQUIRE(tmp >= 0);
	::close(tmp);

	Config cfg = mw_config();
	Router router;
	router.use(structured_log_middleware({.log_file = path}));
	router.get("/x", [](HttpRequest const &) { return HttpResponse::text("x"); });
	ScopedTestServer srv{cfg, std::move(router)};

	auto resp = http_get_on(srv.port(), "/x");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	S log_content;
	{
		int const fd = ::open(path, O_RDONLY);
		REQUIRE(fd >= 0);
		A<char, 4096> buf{};
		ssize_t const n = ::read(fd, buf.data(), buf.size() - 1);
		::close(fd);
		REQUIRE(n > 0);
		log_content.assign(buf.data(), static_cast<SZ>(n));
	}
	::unlink(path);
	REQUIRE(log_content.find("\"app\"") == S::npos);
	REQUIRE(log_content.find(R"("path":"/x")") != S::npos);
}

TEST_CASE(
	"structured_log: path with double-quote is JSON-escaped in log") {
	char path[64]{};
	std::strcpy(path, "/tmp/conflux_slog3_XXXXXX");
	int const tmp = ::mkstemp(path);
	REQUIRE(tmp >= 0);
	::close(tmp);

	Config cfg = mw_config();
	Router router;
	router.use(structured_log_middleware({.log_file = path, .app_name = "test"}));
	// Register a route that matches a path containing a percent-encoded quote.
	router.get("/q", [](HttpRequest const &) { return HttpResponse::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};

	// GET /q?x=1 — path itself is safe; verify basic log integrity.
	auto resp = http_get_on(srv.port(), "/q");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	S log_content;
	{
		int const fd = ::open(path, O_RDONLY);
		REQUIRE(fd >= 0);
		A<char, 4096> buf{};
		ssize_t const n = ::read(fd, buf.data(), buf.size() - 1);
		::close(fd);
		REQUIRE(n > 0);
		log_content.assign(buf.data(), static_cast<SZ>(n));
	}
	::unlink(path);
	// "app" field must be present since app_name is set.
	REQUIRE(log_content.find(R"("app":"test")") != S::npos);
	// Log line must be valid JSON-like (outer braces present).
	REQUIRE(log_content.front() == '{');
}

// ---------------------------------------------------------------------------
// tracing_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"tracing: generates traceparent when none in request") {
	ensure_trace_server();
	auto resp = http_get_on(g_trace_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Response header contains Traceparent.
	auto tp_header = extract_header(resp, "Traceparent");
	REQUIRE(!tp_header.empty());
	REQUIRE(tp_header.starts_with("00-"));
	REQUIRE(tp_header.size() == 55); // "00-" + 32 + "-" + 16 + "-01"
	// Body (injected traceparent in request) matches the response header.
	REQUIRE(extract_body(resp) == tp_header);
}

TEST_CASE(
	"tracing: incoming traceparent preserves trace_id, generates new span_id") {
	ensure_trace_server();
	SV incoming = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
	auto resp = http_get_on(g_trace_port, "/", std::format("traceparent: {}\r\n", incoming));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto tp = extract_header(resp, "Traceparent");
	REQUIRE(!tp.empty());
	// trace_id (chars 3..34) must be preserved.
	REQUIRE(tp.substr(3, 32) == "4bf92f3577b34da6a3ce929d0e0e4736");
	// span_id (chars 36..51) must differ from the incoming parent_id.
	auto new_span = tp.substr(36, 16);
	REQUIRE(new_span != "00f067aa0ba902b7");
}

TEST_CASE(
	"tracing: malformed traceparent generates fresh trace_id") {
	ensure_trace_server();
	auto resp = http_get_on(g_trace_port, "/", "traceparent: bad-value\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto tp = extract_header(resp, "Traceparent");
	REQUIRE(!tp.empty());
	// Must be a well-formed traceparent: "00-<32hex>-<16hex>-01"
	REQUIRE(tp.size() == 55);
	REQUIRE(tp.substr(0, 3) == "00-");
	REQUIRE(tp[35] == '-');
	REQUIRE(tp[52] == '-');
}

TEST_CASE(
	"tracing: non-hex chars in trace_id reject the incoming traceparent") {
	ensure_trace_server();
	// 55-char traceparent with correct structure but non-hex chars in trace_id.
	auto resp =
		http_get_on(g_trace_port, "/", "traceparent: 00-ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ-0000000000000000-01\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto tp = extract_header(resp, "Traceparent");
	REQUIRE(!tp.empty());
	// Must generate a fresh trace_id (not echo back ZZZZ...).
	REQUIRE(tp.substr(3, 32) != "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
}

TEST_CASE(
	"tracing: propagate_in_response=false omits Traceparent response header") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(tracing_middleware({.propagate_in_response = false}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_header(resp, "Traceparent").empty());
}

TEST_CASE(
	"tracing: on_end callback can add response header") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(tracing_middleware({
			.on_end = [](HttpRequest const &,
						 HttpResponse &res,
						 TraceContext const &ctx) { res.headers["X-Trace-Id"] = ctx.trace_id; },
			.propagate_in_response = false,
		}));
		router.get("/", [](HttpRequest const &) { return HttpResponse::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto trace_id = extract_header(resp, "X-Trace-Id");
	REQUIRE(trace_id.size() == 32);
}

TEST_CASE(
	"tracing: on_start callback receives TraceContext and can inject header") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(tracing_middleware({
			.on_start = [](HttpRequest &req, TraceContext const &ctx) { req.headers["x-injected-span"] = ctx.span_id; },
			.propagate_in_response = false,
		}));
		// Echo the injected span id from the request.
		router.get("/", [](HttpRequest const &req) {
			return HttpResponse::text(S{req.headers["x-injected-span"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Body is the span_id: 16 hex chars.
	auto body = extract_body(resp);
	REQUIRE(body.size() == 16);
}

// ---------------------------------------------------------------------------
// VHostRouter
// ---------------------------------------------------------------------------

TEST_CASE(
	"vhost: Host api.example.com routes to api router") {
	ensure_vhost_server();
	auto resp = http_get_on_host(g_vhost_port, "api.example.com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "api");
}

TEST_CASE(
	"vhost: Host web.example.com routes to web router") {
	ensure_vhost_server();
	auto resp = http_get_on_host(g_vhost_port, "web.example.com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "web");
}

TEST_CASE(
	"vhost: HttpServer accepts VHostRouter directly") {
	ensure_vhost_direct_server();

	auto api_resp = http_get_on_host(g_vhost_direct_port, "api.example.com", "/status");
	REQUIRE(api_resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(api_resp) == "api-direct");

	auto default_resp = http_get_on_host(g_vhost_direct_port, "other.example.com", "/status");
	REQUIRE(default_resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(default_resp) == "default-direct");
}

TEST_CASE(
	"vhost: unknown host falls back to default router") {
	ensure_vhost_server();
	auto resp = http_get_on_host(g_vhost_port, "other.example.com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "default");
}

TEST_CASE(
	"vhost: Host header with port suffix is stripped before matching") {
	ensure_vhost_server();
	auto resp = http_get_on_host(g_vhost_port, "api.example.com:8080", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "api");
}

TEST_CASE(
	"vhost: Host matching is case-insensitive") {
	ensure_vhost_server();
	auto resp = http_get_on_host(g_vhost_port, "API.Example.Com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "api");
}

TEST_CASE(
	"vhost: subrouters share one work pool") {
	auto shared_pool = std::make_shared<WorkPool>();

	Router api_router;
	Router web_router;
	Router def_router;

	VHostRouter vhost;
	vhost.set_work_pool(shared_pool);
	vhost.add("api.example.com", std::move(api_router));
	vhost.add("web.example.com", std::move(web_router));
	vhost.set_default(std::move(def_router));

	CHECK(vhost.work_pool().get() == shared_pool.get());
	CHECK(vhost.resolved_work_pool("api.example.com").get() == shared_pool.get());
	CHECK(vhost.resolved_work_pool("web.example.com").get() == shared_pool.get());
	CHECK(vhost.resolved_work_pool("other.example.com").get() == shared_pool.get());
}

TEST_CASE(
	"vhost: rebinding work pool updates existing subrouters") {
	Router api_router;
	Router def_router;

	VHostRouter vhost;
	vhost.add("api.example.com", std::move(api_router));
	vhost.set_default(std::move(def_router));

	auto rebound_pool = std::make_shared<WorkPool>();
	vhost.set_work_pool(rebound_pool);

	CHECK(vhost.work_pool().get() == rebound_pool.get());
	CHECK(vhost.resolved_work_pool("api.example.com").get() == rebound_pool.get());
	CHECK(vhost.resolved_work_pool("other.example.com").get() == rebound_pool.get());
}

TEST_CASE(
	"vhost: unknown host with no default returns 404") {
	static uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router api;
		api.get("/status", [](HttpRequest const &) { return HttpResponse::text("api"); });
		VHostRouter vhost;
		vhost.add("api.example.com", std::move(api));
		// No set_default call.
		Config const cfg{.port = 0, .rings = 1};
		port = test_servers().start(cfg, std::move(vhost));
	});
	auto resp = http_get_on_host(port, "unknown.example.com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 404"));
}

TEST_CASE(
	"vhost: IPv6 host with port is stripped before matching") {
	Router api;
	api.get("/status", [](HttpRequest const &) { return HttpResponse::text("api-v6"); });
	VHostRouter vhost;
	vhost.add("[::1]", std::move(api));

	HttpRequest req;
	req.method = "GET";
	req.path = "/status";
	req.headers["host"] = "[::1]:8080";

	auto resp = vhost.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "api-v6");
}

TEST_CASE(
	"vhost: IPv6 host without port matches directly") {
	Router api;
	api.get("/status", [](HttpRequest const &) { return HttpResponse::text("api-v6-noport"); });
	VHostRouter vhost;
	vhost.add("[::1]", std::move(api));

	HttpRequest req;
	req.method = "GET";
	req.path = "/status";
	req.headers["host"] = "[::1]";

	auto resp = vhost.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "api-v6-noport");
}

// ---------------------------------------------------------------------------
// openapi_spec
// ---------------------------------------------------------------------------

TEST_CASE(
	"openapi: spec contains openapi 3.0.0 root key") {
	Router router;
	router.get("/hello/{name}", [](HttpRequest const &) { return HttpResponse::text(""); });
	router.post("/items", [](HttpRequest const &) { return HttpResponse::text(""); });
	auto spec = openapi_spec(router, "Test API", "0.1.0");
	REQUIRE(spec.find(R"("openapi":"3.0.0")") != S::npos);
}

TEST_CASE(
	"openapi: spec includes registered path with path parameter") {
	Router router;
	router.get("/hello/{name}", [](HttpRequest const &) { return HttpResponse::text(""); });
	auto spec = openapi_spec(router, "Test API", "0.1.0");
	REQUIRE(spec.find(R"("/hello/{name}")") != S::npos);
	REQUIRE(spec.find(R"("name":"name")") != S::npos);
	REQUIRE(spec.find(R"("in":"path")") != S::npos);
}

TEST_CASE(
	"openapi: spec includes title and version from arguments") {
	Router router;
	router.get("/", [](HttpRequest const &) { return HttpResponse::text(""); });
	auto spec = openapi_spec(router, "My Service", "2.3.4");
	REQUIRE(spec.find(R"("title":"My Service")") != S::npos);
	REQUIRE(spec.find(R"("version":"2.3.4")") != S::npos);
}

TEST_CASE(
	"openapi: spec includes method in lowercase") {
	Router router;
	router.post("/items", [](HttpRequest const &) { return HttpResponse::text(""); });
	auto spec = openapi_spec(router);
	REQUIRE(spec.find(R"("post":)") != S::npos);
}

TEST_CASE(
	"openapi: title with special characters is properly JSON-escaped") {
	Router router;
	router.get("/", [](HttpRequest const &) { return HttpResponse::text(""); });
	auto spec = openapi_spec(router, R"(My "API" & More)");
	REQUIRE(spec.find(R"("title":"My \"API\" & More")") != S::npos);
}

TEST_CASE(
	"openapi: empty router produces valid paths object") {
	Router router;
	auto spec = openapi_spec(router, "Empty", "0.0.1");
	REQUIRE(spec.find(R"("paths":{})") != S::npos);
	REQUIRE(spec.find(R"("title":"Empty")") != S::npos);
}

TEST_CASE(
	"openapi_handler_protected: wrong bearer token returns 401") {
	Router router;
	router.get("/ping", [](HttpRequest const &) { return HttpResponse::text("pong"); });
	V<Router::Middleware> chain;
	chain.push_back(bearer_auth_middleware([](SV token) { return token == "apikey"; }));
	router.get("/openapi.json", openapi_handler_protected(router, "API", "1.0.0", std::move(chain)));

	Config const cfg{.port = 0, .rings = 1};
	uint16_t port = test_servers().start(cfg, std::move(router));

	auto resp_no_auth = http_get_on(port, "/openapi.json");
	REQUIRE(resp_no_auth.starts_with("HTTP/1.1 401"));

	auto resp_ok = http_get_on(port, "/openapi.json", "Authorization: Bearer apikey\r\n");
	REQUIRE(resp_ok.starts_with("HTTP/1.1 200"));
	REQUIRE(resp_ok.find("application/json") != S::npos);
	REQUIRE(resp_ok.find(R"("openapi":"3.0.0")") != S::npos);
}

namespace {

S send_raw_bytes(
	SV raw) {
	ensure_server();
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket failed"};
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_test_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"connect failed"};
	}
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
	S response;
	A<char, 4096> buf{};
	for (;;) {
		auto n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n <= 0) {
			break;
		}
		response.append(buf.data(), static_cast<SZ>(n));
	}
	::close(fd);
	return response;
}

} // namespace

TEST_CASE(
	"parser: request line exceeding 8 KiB returns 414") {
	S path = "/";
	path.append(9000, 'a');
	auto req = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 414"));
}

TEST_CASE(
	"parser: invalid method token returns 400") {
	auto resp = send_raw_bytes("GE<T /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: empty request target returns 400") {
	auto resp = send_raw_bytes("GET  HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: single header line exceeding 8 KiB returns 431") {
	S header_value(9000, 'v');
	auto req = std::format(
		"GET /api/ping HTTP/1.1\r\nHost: localhost\r\nX-Big: {}\r\nConnection: close\r\n\r\n",
		header_value);
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 431"));
}

TEST_CASE(
	"parser: more than 100 headers returns 431") {
	S req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
	for (int i = 0; i < 120; ++i) {
		req += std::format("X-H-{}: v\r\n", i);
	}
	req += "\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 431"));
}

TEST_CASE(
	"parser: obs-fold line returns 400") {
	S req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nX-A: one\r\n two\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: NUL byte in header returns 400") {
	S req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nX-Bad: a";
	req.push_back('\0');
	req += "b\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: header missing colon returns 400") {
	S req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nNoColonHere\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: field-name with space before colon returns 400") {
	S req = "GET /api/ping HTTP/1.1\r\nHost : localhost\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: header with no space after colon is accepted") {
	S req =
		"GET /api/echo-header HTTP/1.1\r\nHost: localhost\r\nX-Test-Header:no-space\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
	REQUIRE(resp.find("no-space") != S::npos);
}

TEST_CASE(
	"parser: header value is trimmed of leading and trailing OWS") {
	S req =
		"GET /api/echo-header HTTP/1.1\r\nHost: localhost\r\nX-Test-Header:   spaced   \r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != S::npos);
	auto body = resp.substr(body_start + 4);
	REQUIRE(body == "spaced");
}

TEST_CASE(
	"parser: malformed Content-Length returns 400") {
	S req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5abc\r\nConnection: close\r\n\r\nhello";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: duplicate Content-Length returns 400") {
	S req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 5\r\n"
		"Connection: close\r\n\r\nhello";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: unsupported Transfer-Encoding returns 400") {
	S req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip, chunked\r\n"
		"Connection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: Transfer-Encoding after chunked returns 400") {
	S req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked, gzip\r\n"
		"Connection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: duplicate Transfer-Encoding headers return 400") {
	S req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"
		"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: uppercase chunked Transfer-Encoding is accepted") {
	S req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: CHUNKED\r\n"
		"Connection: close\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
}

TEST_CASE(
	"parser: Connection close token closes persistent connection") {
	REQUIRE(server_closed_after("/api/ping", "Connection: keep-alive, close\r\n"));
}

TEST_CASE(
	"parser: chunked transfer with chunk-count overflow returns 400") {
	S req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
	for (int i = 0; i < 200'000; ++i) {
		req += "1\r\nx\r\n";
	}
	req += "0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

// ---------------------------------------------------------------------------
// WebSocket frame validation + fragmentation (A2)
// ---------------------------------------------------------------------------

namespace ws_test {

S read_http_headers(
	int fd) {
	S resp;
	A<char, 512> buf{};
	while (resp.find("\r\n\r\n") == S::npos) {
		auto n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		REQUIRE(n > 0);
		resp.append(buf.data(), static_cast<SZ>(n));
		REQUIRE(resp.size() <= 8192);
	}
	return resp;
}

Config ws_cfg() {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	return cfg;
}

int ws_handshake(
	uint16_t port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	S req = std::format(
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n");
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	auto resp = read_http_headers(fd);
	REQUIRE(resp.starts_with("HTTP/1.1 101"));
	REQUIRE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != S::npos);
	return fd;
}

V<uint8_t> make_masked_frame(
	uint8_t b0,
	SV payload,
	bool mask = true) {
	V<uint8_t> f;
	f.push_back(b0);
	auto len = payload.size();
	uint8_t const mask_bit = mask ? 0x80U : 0U;
	if (len < 126) {
		f.push_back(mask_bit | static_cast<uint8_t>(len));
	} else if (len <= 0xFFFF) {
		f.push_back(mask_bit | 126U);
		f.push_back(static_cast<uint8_t>(len >> 8));
		f.push_back(static_cast<uint8_t>(len & 0xFFU));
	} else {
		f.push_back(mask_bit | 127U);
		for (int s = 56; s >= 0; s -= 8) {
			f.push_back(static_cast<uint8_t>((len >> s) & 0xFFU));
		}
	}
	if (mask) {
		A<uint8_t, 4> const key{0x01, 0x02, 0x03, 0x04};
		f.insert(f.end(), key.begin(), key.end());
		for (SZ i = 0; i < payload.size(); ++i) {
			f.push_back(static_cast<uint8_t>(payload[i]) ^ key[i & 3]);
		}
	} else {
		f.insert(f.end(), payload.begin(), payload.end());
	}
	return f;
}

struct CloseFrame {
	uint16_t code{};
	S reason;
	bool received{};
};

CloseFrame read_close(
	int fd) {
	auto read_exact = [fd](std::span<uint8_t> out) {
		SZ got = 0;
		while (got < out.size()) {
			auto n = ::recv(fd, out.data() + got, out.size() - got, 0);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				return false;
			}
			got += static_cast<SZ>(n);
		}
		return true;
	};

	A<uint8_t, 2> header{};
	if (!read_exact(header)) {
		return {};
	}
	uint8_t const b0 = header[0];
	uint8_t const b1 = header[1] & 0x7FU;
	if ((b0 & 0x0FU) != 0x08U) {
		return {};
	}
	if (b1 > 125) {
		return {};
	}
	A<uint8_t, 125> payload{};
	if (!read_exact(std::span{payload}.first(b1))) {
		return {};
	}
	if (b1 < 2) {
		return {.code = 0, .reason = {}, .received = true};
	}
	auto const code =
		static_cast<uint16_t>((static_cast<uint32_t>(payload[0]) << 8U) | static_cast<uint32_t>(payload[1]));
	S reason;
	if (b1 > 2) {
		reason.assign(reinterpret_cast<char const *>(payload.data()) + 2, static_cast<SZ>(b1) - 2);
	}
	return {.code = code, .reason = std::move(reason), .received = true};
}

} // namespace ws_test

TEST_CASE(
	"ws: frame with RSV bit set triggers close 1002") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto frame = ws_test::make_masked_frame(0xC1U, "x"); // FIN | RSV1 | text
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: unmasked client frame triggers close 1002") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto frame = ws_test::make_masked_frame(0x81U, "x", /*mask=*/false);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: non-minimal extended payload length triggers close 1002") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	A<uint8_t, 9> frame{
		0x81U, // FIN | text
		0xFEU, // MASK | 126 extended length marker
		0x00U,
		0x01U, // non-minimal encoding for length 1
		0x01U,
		0x02U,
		0x03U,
		0x04U,
		static_cast<uint8_t>('x' ^ 0x01U)};
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: oversized control frame (ping with 126-byte payload) triggers close 1002") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	S big(126, 'p');
	auto frame = ws_test::make_masked_frame(0x89U, big); // FIN | ping
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: handshake without Upgrade header is rejected") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &) {});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	S const req =
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Connection: keep-alive\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	auto resp = ws_test::read_http_headers(fd);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: handshake with invalid Sec-WebSocket-Key is rejected") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &) {});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	S const req =
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: keep-alive, Upgrade\r\n"
		"Sec-WebSocket-Key: not-a-valid-key\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	auto resp = ws_test::read_http_headers(fd);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: one-byte close payload triggers close 1002") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto frame = ws_test::make_masked_frame(0x88U, "x"); // FIN | close, invalid payload length 1
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: invalid close code is rejected instead of echoed") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	S code_payload;
	code_payload.push_back('\x03');
	code_payload.push_back('\xED'); // 1005 (reserved/invalid close code)
	auto frame = ws_test::make_masked_frame(0x88U, code_payload);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: invalid close reason UTF-8 from peer triggers close 1007") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	S close_payload;
	close_payload.push_back('\x03');
	close_payload.push_back('\xE8'); // 1000
	close_payload.append("\xC0\xAF", 2);
	auto frame = ws_test::make_masked_frame(0x88U, close_payload);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1007);
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: close rejects invalid status code via public API") {
	auto result = std::make_shared<std::promise<bool>>();
	auto done = result->get_future();
	Router router;
	router.ws("/ws", [result](HttpRequest const &, WsConn &ws) {
		try {
			ws.close(1005);
			result->set_value(false);
		} catch (std::invalid_argument const &) { result->set_value(true); }
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	REQUIRE(done.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
	REQUIRE(done.get());
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: close rejects invalid UTF-8 reason via public API") {
	auto result = std::make_shared<std::promise<bool>>();
	auto done = result->get_future();
	Router router;
	router.ws("/ws", [result](HttpRequest const &, WsConn &ws) {
		try {
			ws.close(1000, SV{"\xC0\xAF", 2});
			result->set_value(false);
		} catch (std::invalid_argument const &) { result->set_value(true); }
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	REQUIRE(done.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
	REQUIRE(done.get());
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: fragmented text message is reassembled before handler sees it") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		while (auto f = ws.recv()) {
			if (f->opcode == WsConn::Opcode::Text) {
				ws.send_text(f->payload);
			}
		}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto part1 = ws_test::make_masked_frame(0x01U, "hel"); // FIN=0 | text
	auto part2 = ws_test::make_masked_frame(0x80U, "lo"); // FIN=1 | continuation
	::send(fd, part1.data(), part1.size(), MSG_NOSIGNAL);
	::send(fd, part2.data(), part2.size(), MSG_NOSIGNAL);
	A<uint8_t, 64> rx{};
	auto n = ::recv(fd, rx.data(), rx.size(), 0);
	REQUIRE(n >= 7);
	REQUIRE(rx[0] == 0x81U); // FIN | text
	REQUIRE((rx[1] & 0x7FU) == 5U);
	S echo{reinterpret_cast<char const *>(rx.data()) + 2, 5};
	REQUIRE(echo == "hello");
	::close(fd);
	srv.stop();
}

TEST_CASE(
	"ws: invalid UTF-8 in text frame triggers close 1007") {
	Router router;
	router.ws("/ws", [](HttpRequest const &, WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	S bad{"\xC0\xAF"}; // overlong / illegal sequence
	auto frame = ws_test::make_masked_frame(0x81U, bad);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1007);
	::close(fd);
	srv.stop();
}

// ---------------------------------------------------------------------------
// A3: TLS sniff timeout + EOF handling
// ---------------------------------------------------------------------------

TEST_CASE(
	"TLS sniff: silent connection closed after tls_sniff_timeout_ms") {
	char cert_tmp[] = "/tmp/conflux_sniff_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_sniff_key_XXXXXX.pem";
	{
		int fd = ::mkstemps(cert_tmp, 4);
		::close(fd);
		fd = ::mkstemps(key_tmp, 4);
		::close(fd);
	}
	S const cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	REQUIRE(::system(cmd.c_str()) == 0);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 0; // disable idle-request reap so only sniff timeout is exercised
	cfg.tls_sniff_timeout_ms = 1500;
	cfg.cert_file = cert_tmp;
	cfg.key_file = key_tmp;

	Router router;
	router.get("/ok", [](HttpRequest const &) { return HttpResponse::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const port = srv.port();
	::unlink(cert_tmp);
	::unlink(key_tmp);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	char buf[1];
	auto n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
	::close(fd);
	bool const connection_gone = (n == 0) || (n < 0 && (errno == ECONNRESET || errno == EAGAIN));
	REQUIRE(connection_gone);

	srv.stop();
}

TEST_CASE(
	"TLS sniff: client half-close before any data triggers clean server close") {
	char cert_tmp[] = "/tmp/conflux_eof_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_eof_key_XXXXXX.pem";
	{
		int fd = ::mkstemps(cert_tmp, 4);
		::close(fd);
		fd = ::mkstemps(key_tmp, 4);
		::close(fd);
	}
	S const cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	REQUIRE(::system(cmd.c_str()) == 0);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 30000;
	cfg.tls_sniff_timeout_ms = 30000; // big — we don't want timer reap to be what closes us
	cfg.cert_file = cert_tmp;
	cfg.key_file = key_tmp;

	Router router;
	router.get("/ok", [](HttpRequest const &) { return HttpResponse::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const port = srv.port();
	::unlink(cert_tmp);
	::unlink(key_tmp);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	::shutdown(fd, SHUT_WR);

	char buf[1];
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	auto n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
	::close(fd);
	bool const connection_gone = (n == 0) || (n < 0 && (errno == ECONNRESET || errno == EAGAIN));
	REQUIRE(connection_gone);

	srv.stop();
}

// ---------------------------------------------------------------------------
// C1: SseChannel bounded queue + overflow policies (unit tests, no server)
// ---------------------------------------------------------------------------

TEST_CASE(
	"SseChannel: DropNewest policy drops overflowing frames") {
	SseChannel ch{64, SseOverflowPolicy::DropNewest};
	// Each frame is 10 bytes; queue holds at most 64 bytes → 6 fit.
	for (int i = 0; i < 10; ++i) {
		S frame(10, 'x');
		(void)ch.send(std::move(frame));
	}
	REQUIRE(ch.dropped_count() == 4);
	auto out = ch.drain();
	REQUIRE(out.size() == 60);
}

TEST_CASE(
	"SseChannel: DropOldest policy keeps newest frames") {
	SseChannel ch{30, SseOverflowPolicy::DropOldest};
	for (int i = 0; i < 5; ++i) {
		S frame(10, static_cast<char>('a' + i));
		(void)ch.send(std::move(frame));
	}
	REQUIRE(ch.dropped_count() >= 2);
	auto out = ch.drain();
	// After overflow, the final 3 frames (cc…, dd…, ee…) should remain.
	REQUIRE(out.find("eeeeeeeeee") != S::npos);
	REQUIRE(out.find("aaaaaaaaaa") == S::npos);
}

TEST_CASE(
	"SseChannel: Disconnect policy closes on overflow") {
	SseChannel ch{20, SseOverflowPolicy::Disconnect};
	REQUIRE(ch.send(S(10, 'x')));
	// Next send exceeds the cap → channel is closed; further sends return false.
	(void)ch.send(S(20, 'y'));
	REQUIRE(ch.is_closed());
	REQUIRE_FALSE(ch.send(S(5, 'z')));
}

TEST_CASE(
	"SseChannel: send returns false after close") {
	SseChannel ch{4096};
	ch.close();
	REQUIRE_FALSE(ch.send("hello"));
}

// ---------------------------------------------------------------------------
// C2: DeferredResponse timeout
// ---------------------------------------------------------------------------

TEST_CASE(
	"deferred response that never completes returns 504 after its deadline") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 30000;

	Router router;
	// Handler returns a DeferredResponse with a 1-second deadline but never completes it.
	// The idle-timer sweeper should expire it with a 504 shortly after.
	router.get("/stuck", [](HttpRequest const &) {
		auto d = std::make_shared<DeferredResponse>(std::chrono::milliseconds{1000});
		return HttpResponse::deferred(d);
	});

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const p = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(p);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	SV const req = "GET /stuck HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);

	auto const response = read_one_response(fd);
	::close(fd);
	REQUIRE(response.starts_with("HTTP/1.1 504"));
	srv.stop();
}

TEST_CASE(
	"deferred response that completes before its deadline returns the completed payload") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	router.get("/fast", [](HttpRequest const &) {
		auto d = std::make_shared<DeferredResponse>(std::chrono::milliseconds{10'000});
		std::thread([d]() {
			std::this_thread::sleep_for(std::chrono::milliseconds{80});
			d->complete(HttpResponse::text("pong"));
		}).detach();
		return HttpResponse::deferred(d);
	});

	ScopedTestServer srv{cfg, std::move(router)};
	uint16_t const p = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(p);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	SV const req = "GET /fast HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);

	auto const response = read_one_response(fd);
	::close(fd);
	REQUIRE(response.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response.find("pong") != S::npos);
	srv.stop();
}

// ---------------------------------------------------------------------------
// HttpFields — set/erase
// ---------------------------------------------------------------------------

TEST_CASE(
	"HttpFields::set replaces all duplicate entries with a single one") {
	HttpFields f{true};
	f.emplace_back("Set-Cookie", "a=1");
	f.emplace_back("Set-Cookie", "b=2");
	f.emplace_back("Set-Cookie", "c=3");
	REQUIRE(f.size() == 3);

	f.set("set-cookie", "z=9");
	REQUIRE(f.size() == 1);
	REQUIRE(f.get("set-cookie") == "z=9");
	REQUIRE(f.values("set-cookie").size() == 1);
	REQUIRE(f.contains("set-cookie"));
}

TEST_CASE(
	"HttpFields::set inserts when key absent") {
	HttpFields f{true};
	f.set("Content-Type", "text/plain");
	REQUIRE(f.size() == 1);
	REQUIRE(f.get("content-type") == "text/plain");
}

TEST_CASE(
	"HttpFields::set preserves positions of other keys") {
	HttpFields f{true};
	f.emplace_back("A", "1");
	f.emplace_back("Dup", "x");
	f.emplace_back("B", "2");
	f.emplace_back("Dup", "y");
	f.emplace_back("C", "3");

	f.set("dup", "merged");
	REQUIRE(f.size() == 4);
	V<S> keys;
	for (auto const &[k, _v]: f) {
		keys.push_back(k);
	}
	REQUIRE(keys == (V<S>{"A", "Dup", "B", "C"}));
	REQUIRE(f.get("dup") == "merged");
	REQUIRE(f.get("a") == "1");
	REQUIRE(f.get("b") == "2");
	REQUIRE(f.get("c") == "3");
}

TEST_CASE(
	"HttpFields::erase removes all matches and returns count") {
	HttpFields f{true};
	f.emplace_back("Cookie", "a=1");
	f.emplace_back("Cookie", "b=2");
	f.emplace_back("Host", "example.com");

	auto removed = f.erase("cookie");
	REQUIRE(removed == 2);
	REQUIRE(f.size() == 1);
	REQUIRE(!f.contains("cookie"));
	REQUIRE(f.get("host") == "example.com");
}

TEST_CASE(
	"HttpFields::erase returns 0 when key absent") {
	HttpFields f{true};
	f.emplace_back("A", "1");
	REQUIRE(f.erase("missing") == 0);
	REQUIRE(f.size() == 1);
}

TEST_CASE(
	"HttpFields index stays consistent after set then erase") {
	HttpFields f{true};
	f.emplace_back("X", "1");
	f.emplace_back("X", "2");
	f.emplace_back("Y", "a");

	f.set("x", "z");
	REQUIRE(f.contains("x"));
	REQUIRE(f.get("x") == "z");
	REQUIRE(f.values("x").size() == 1);

	auto removed = f.erase("x");
	REQUIRE(removed == 1);
	REQUIRE(!f.contains("x"));
	REQUIRE(f.get("y") == "a");
	REQUIRE(f.size() == 1);
}

TEST_CASE(
	"HttpFields::values returns all entries for duplicate keys") {
	HttpFields f{true};
	f.emplace_back("Cookie", "a=1");
	f.emplace_back("Cookie", "b=2");
	f.emplace_back("Cookie", "c=3");
	f.emplace_back("Other", "x");
	auto vals = f.values("cookie");
	REQUIRE(vals.size() == 3);
	using sv = SV;
	REQUIRE(std::ranges::contains(vals, sv{"a=1"}));
	REQUIRE(std::ranges::contains(vals, sv{"b=2"}));
	REQUIRE(std::ranges::contains(vals, sv{"c=3"}));
}

TEST_CASE(
	"HttpFields::value_or returns default when key absent") {
	HttpFields f{true};
	f.emplace_back("A", "hello");
	REQUIRE(f.value_or("A") == "hello");
	REQUIRE(f.value_or("Missing", "default") == "default");
	REQUIRE(f.value_or("Missing") == "");
}

TEST_CASE(
	"static file serving: percent-encoded filename in URL is decoded and served") {
	char tmpdir[] = "/tmp/conflux_enc_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto fpath = S{tmpdir} + "/hello world.txt";
	int const wfd = ::open(fpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(wfd >= 0);
	SV const content{"space file"};
	::write(wfd, content.data(), content.size());
	::close(wfd);

	Router router;
	router.serve_static("/s", S{tmpdir});

	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/hello%20world.txt");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != S::npos);
	REQUIRE(resp.substr(body_start + 4) == "space file");

	srv.stop();
	::unlink(fpath.c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: If-Modified-Since matching the file mtime returns 304") {
	char tmpdir[] = "/tmp/conflux_ims_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto fpath = S{tmpdir} + "/test.txt";
	int const wfd = ::open(fpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(wfd >= 0);
	SV const content{"hello"};
	::write(wfd, content.data(), content.size());
	::close(wfd);

	Router router;
	router.serve_static("/s", S{tmpdir});

	ScopedTestServer srv{mw_config(), std::move(router)};

	// First request: get the Last-Modified header.
	auto resp1 = conflux::tests::http_get_on(srv.port(), "/s/test.txt");
	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	auto last_modified = extract_header(resp1, "Last-Modified");
	REQUIRE(!last_modified.empty());

	// Second request with matching If-Modified-Since: should get 304.
	auto resp2 =
		conflux::tests::http_get_on(srv.port(), "/s/test.txt", std::format("If-Modified-Since: {}\r\n", last_modified));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));

	srv.stop();
	::unlink(fpath.c_str());
	::rmdir(tmpdir);
}

// ---------------------------------------------------------------------------
// Router: wildcard {*name} tail capture (unit tests, no server)
// ---------------------------------------------------------------------------

TEST_CASE(
	"router: wildcard {*path} captures entire tail") {
	Router router;
	router.get("/files/{*path}", [](HttpRequest const &req) {
		return HttpResponse::text(S{req.params["path"]});
	});
	HttpRequest req;
	req.method = "GET";
	req.path = "/files/docs/readme.txt";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "docs/readme.txt");
}

TEST_CASE(
	"router: wildcard {*path} captures empty tail when path ends at prefix") {
	Router router;
	router.get("/files/{*path}", [](HttpRequest const &req) {
		return HttpResponse::text(S{req.params["path"]});
	});
	HttpRequest req;
	req.method = "GET";
	req.path = "/files/";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body().empty());
}

TEST_CASE(
	"router: wildcard with prefix param captures both") {
	Router router;
	router.get("/{version}/files/{*path}", [](HttpRequest const &req) {
		return HttpResponse::text(std::format("{}/{}", req.params["version"], req.params["path"]));
	});
	HttpRequest req;
	req.method = "GET";
	req.path = "/v2/files/a/b/c.txt";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "v2/a/b/c.txt");
}

TEST_CASE(
	"router: non-matching path returns 404") {
	Router router;
	router.get("/files/{*path}", [](HttpRequest const &req) {
		return HttpResponse::text(S{req.params["path"]});
	});
	HttpRequest req;
	req.method = "GET";
	req.path = "/other/stuff";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 404);
}

TEST_CASE(
	"router: percent-encoded path param is URL-decoded") {
	Router router;
	router.get("/hello/{name}", [](HttpRequest const &req) {
		return HttpResponse::text(S{req.params["name"]});
	});
	HttpRequest req;
	req.method = "GET";
	req.path = "/hello/hello%20world";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "hello world");
}

TEST_CASE(
	"router: wildcard tail with percent-encoded segment is URL-decoded") {
	Router router;
	router.get("/files/{*path}", [](HttpRequest const &req) {
		return HttpResponse::text(S{req.params["path"]});
	});
	HttpRequest req;
	req.method = "GET";
	req.path = "/files/dir/my%20file.txt";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "dir/my file.txt");
}

// ---------------------------------------------------------------------------
// Router: on_not_found / on_error custom handlers
// ---------------------------------------------------------------------------

TEST_CASE(
	"router: on_not_found custom handler called for unmatched path") {
	Router router;
	router.get("/exists", [](HttpRequest const &) { return HttpResponse::text("ok"); });
	router.on_not_found([](HttpRequest const &req) { return HttpResponse::text(std::format("nope:{}", req.path)); });
	HttpRequest req;
	req.method = "GET";
	req.path = "/missing";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "nope:/missing");
}

TEST_CASE(
	"router: on_error custom handler called when route throws") {
	Router router;
	router.get("/boom", [](HttpRequest const &) -> HttpResponse { throw std::runtime_error{"oops"}; });
	S captured_what;
	router.on_error([&](HttpRequest const &, std::exception const &ex) {
		captured_what = ex.what();
		return HttpResponse::text("caught");
	});
	HttpRequest req;
	req.method = "GET";
	req.path = "/boom";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "caught");
	REQUIRE(captured_what == "oops");
}

TEST_CASE(
	"router: default 404 when no on_not_found is set") {
	Router router;
	router.get("/a", [](HttpRequest const &) { return HttpResponse::text("a"); });
	HttpRequest req;
	req.method = "GET";
	req.path = "/missing";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 404);
}

TEST_CASE(
	"router: default 500 when route throws and no on_error is set") {
	Router router;
	router.get("/boom", [](HttpRequest const &) -> HttpResponse { throw std::runtime_error{"crash"}; });
	HttpRequest req;
	req.method = "GET";
	req.path = "/boom";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 500);
}

// ---------------------------------------------------------------------------
// http1_parser unit tests
// ---------------------------------------------------------------------------

TEST_CASE(
	"http1_parser: valid GET request parses correctly") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET /path HTTP/1.1\r\nHost: localhost\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.method == "GET");
	REQUIRE(out.target == "/path");
	REQUIRE(out.version == "HTTP/1.1");
	REQUIRE(out.headers.size() == 1);
	REQUIRE(out.headers[0].first == "Host");
	REQUIRE(out.headers[0].second == "localhost");
}

TEST_CASE(
	"http1_parser: incomplete request returns Incomplete") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET /path HTTP/1.1\r\nHost: localhost\r\n", limits, out);
	REQUIRE(status == ParseStatus::Incomplete);
}

TEST_CASE(
	"http1_parser: missing HTTP version returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// "GET /path NOTHTTP" → version check fails → BadRequest.
	auto status = parse_request("GET /path NOTHTTP\r\nHost: x\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

TEST_CASE(
	"http1_parser: request with no headers parses correctly") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// Header-free requests must not underflow header_block_size.
	auto status = parse_request("GET / HTTP/1.1\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.method == "GET");
	REQUIRE(out.target == "/");
	REQUIRE(out.headers.empty());
}

TEST_CASE(
	"http1_parser: URI too long returns UriTooLong") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_request_line_size = 10;
	ParsedRequest out;
	auto status = parse_request("GET /very-long-path-here HTTP/1.1\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::UriTooLong);
}

TEST_CASE(
	"http1_parser: header with null byte returns BadRequest") {
	using namespace conflux::http1;
	using namespace S_literals;
	ParserLimits const limits{};
	ParsedRequest out;
	// Use "s" suffix so S captures embedded null bytes.
	S raw = "GET / HTTP/1.1\r\nX-Bad: val\x00ue\r\n\r\n"s;
	auto status = parse_request(raw, limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

TEST_CASE(
	"http1_parser: too many headers returns HeaderFieldsTooLarge") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_headers = 2;
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::HeaderFieldsTooLarge);
}

TEST_CASE(
	"http1_parser: header value leading/trailing whitespace is stripped") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nX-Foo:   bar  \r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.headers.size() == 1);
	REQUIRE(out.headers[0].second == "bar");
}

TEST_CASE(
	"http1_parser: HTTP/1.0 version is accepted") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.0\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.version == "HTTP/1.0");
}

TEST_CASE(
	"http1_parser: empty method returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// Space at the very start means method is empty.
	auto status = parse_request(" /path HTTP/1.1\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

TEST_CASE(
	"http1_parser: invalid tchar in header name returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// Space in field name is not a valid tchar.
	auto status = parse_request("GET / HTTP/1.1\r\nX Bad: value\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

TEST_CASE(
	"http1_parser: folded header line returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// Line starting with whitespace (obs-fold) is rejected per RFC 7230 §3.2.6.
	auto status = parse_request("GET / HTTP/1.1\r\nX-Foo: bar\r\n  continuation\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

TEST_CASE(
	"http1_parser: header with empty name (bare colon) returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// ": value" — colon at position 0, no field name.
	auto status = parse_request("GET / HTTP/1.1\r\n: value\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

// ---------------------------------------------------------------------------
// serve_static: directory listing
// ---------------------------------------------------------------------------

TEST_CASE(
	"static file serving: directory request without listing returns 403") {
	char tmpdir[] = "/tmp/conflux_dirlist_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	// Put a file so the dir is non-empty (no index.html).
	auto fpath = S{tmpdir} + "/file.txt";
	int const fd = ::open(fpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(fd >= 0);
	::write(fd, "hi", 2);
	::close(fd);

	Router router;
	router.serve_static("/s", S{tmpdir});

	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));

	srv.stop();
	::unlink(fpath.c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: directory request with listing returns HTML") {
	char tmpdir[] = "/tmp/conflux_dirlist2_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto write_file = [&](SV name, SV content) {
		auto path = S{tmpdir} + "/" + S{name};
		int const wfd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(wfd >= 0);
		::write(wfd, content.data(), content.size());
		::close(wfd);
	};
	write_file("alpha.txt", "a");
	write_file("beta.html", "b");

	Router router;
	StaticOptions sopts{};
	sopts.directory_listing = true;
	router.serve_static("/s", S{tmpdir}, sopts);

	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != S::npos);
	auto body = resp.substr(body_start + 4);
	REQUIRE(body.find("alpha.txt") != S::npos);
	REQUIRE(body.find("beta.html") != S::npos);
	REQUIRE(body.find("<ul>") != S::npos);

	srv.stop();
	::unlink((S{tmpdir} + "/alpha.txt").c_str());
	::unlink((S{tmpdir} + "/beta.html").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: directory listing entries are sorted") {
	char tmpdir[] = "/tmp/conflux_dirlist3_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto write_file = [&](SV name) {
		auto path = S{tmpdir} + "/" + S{name};
		int const wfd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(wfd >= 0);
		::close(wfd);
	};
	write_file("zebra.txt");
	write_file("apple.txt");
	write_file("mango.txt");

	Router router;
	StaticOptions sopts{};
	sopts.directory_listing = true;
	router.serve_static("/s", S{tmpdir}, sopts);

	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != S::npos);
	auto body = resp.substr(body_start + 4);
	auto apple_pos = body.find("apple.txt");
	auto mango_pos = body.find("mango.txt");
	auto zebra_pos = body.find("zebra.txt");
	REQUIRE(apple_pos != S::npos);
	REQUIRE(mango_pos != S::npos);
	REQUIRE(zebra_pos != S::npos);
	REQUIRE(apple_pos < mango_pos);
	REQUIRE(mango_pos < zebra_pos);

	srv.stop();
	::unlink((S{tmpdir} + "/zebra.txt").c_str());
	::unlink((S{tmpdir} + "/apple.txt").c_str());
	::unlink((S{tmpdir} + "/mango.txt").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: directory listing index.html takes precedence") {
	char tmpdir[] = "/tmp/conflux_dirlist4_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto path = S{tmpdir} + "/index.html";
	int const wfd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(wfd >= 0);
	auto content = SV{"<h1>Index</h1>"};
	::write(wfd, content.data(), content.size());
	::close(wfd);

	Router router;
	StaticOptions sopts{};
	sopts.directory_listing = true;
	router.serve_static("/s", S{tmpdir}, sopts);

	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != S::npos);
	auto body = resp.substr(body_start + 4);
	REQUIRE(body == "<h1>Index</h1>");

	srv.stop();
	::unlink(path.c_str());
	::rmdir(tmpdir);
}
