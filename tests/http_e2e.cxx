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
#include <unistd.h>
#include <zlib.h>

import std;
import conflux.types;

import conflux.crypto;
import conflux.json;
import conflux.net.app.defer;
import conflux.net.async_client;
import conflux.http.extended;
import conflux.net.app;
import conflux.net.auth;
import conflux.net.cache_control;
import conflux.net.compress;
import conflux.net.config;
import conflux.net.cookie_signing;
import conflux.net.cors;
import conflux.net.csrf;
import conflux.net.etag;
import conflux.net.forwarded;
import conflux.net.http.client;
import conflux.net.http.realtime;
import conflux.net.http.static_files;
import conflux.net.http_server;
import conflux.net.ip_filter;
import conflux.net.metrics;
import conflux.net.openapi;
import conflux.net.proxy;
import conflux.net.rate_limit;
import conflux.net.redirect;
import conflux.net.request_id;
import conflux.net.response_cache;
import conflux.net.router;
import conflux.net.security;
import conflux.net.structured_log;
import conflux.net.tracing;
import conflux.net.trailing_slash;
import conflux.net.vhost;
#if CONFLUX_HAS_TLS
import conflux.net.jwt;
#endif
import conflux.net.compress;
import conflux.net.proxy;
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
using namespace conflux::tests;

TEST_CASE(
	"middleware next is one-shot",
	"[middleware]") {
	Router router;
	router.use([](Request const &req, Router::Handler const &next) {
		(void)next(req);
		return next(req);
	});
	router.get("/twice", [](Request const &) { return Response::text("ok"); });

	Request req;
	req.method = "GET";
	req.path = "/twice";
	CHECK_THROWS_AS(router.dispatch(req), std::logic_error);
}

namespace {
namespace chttp = conflux::http;
using conflux::http::HttpClient;
using conflux::http::HttpClientOptions;
using conflux::http::HttpErrorKind;
using conflux::http::HttpTimeouts;

// Actual port chosen by the OS; set once in ensure_server().
std::uint16_t g_test_port = 0;
std::uint16_t g_redirect_follow_source_port = 0;
std::uint16_t g_redirect_follow_target_port = 0;
std::uint16_t g_redirect_follow_async_port = 0;

TEST_CASE(
	"http app: try_server constructs server without throwing",
	"[http][app]") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.get("/", [](Request const &) { return Response::text("ok"); });
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
}

TEST_CASE(
	"http app: facade forwards websocket and static route registration",
	"[http][app]") {
	auto app = chttp::App::default_server();
	auto &ws_app = app.ws("/ws", [](RequestView const &, conflux::http::WsConn &) {});
	CHECK(&ws_app == &app);
	auto &sse_app = app.sse("/events", [](RequestView const &, std::shared_ptr<conflux::http::SseChannel> const &) {});
	CHECK(&sse_app == &app);
	auto &static_app = app.serve_static("/assets", std::filesystem::temp_directory_path().string());
	CHECK(&static_app == &app);

	auto routes = app.routes();
	REQUIRE(routes.size() == 2);
	CHECK(routes[0].method == "GET");
	CHECK(routes[0].path == "/ws");
	CHECK(routes[0].handler_kind == "ws");
	CHECK(routes[1].method == "GET");
	CHECK(routes[1].path == "/events");
	CHECK(routes[1].handler_kind == "sse");
}

TEST_CASE(
	"http app: generic route registration and introspection stay on facade",
	"[http][app]") {
	auto app = chttp::App::default_server();
	auto &added = app.add("REPORT", "/reports/{id}", [](Request const &req) {
		return Response::text(std::string{req.params["id"]});
	});
	CHECK(&added == &app);

	auto &ctx_added = app.add_context(
		"POST",
		"/jobs/{id}",
		[](RequestView const &, chttp::RequestContext const &) -> conflux::work::root::Task<Response> {
			auto [task, source] = conflux::work::root::make_task_source<Response>();
			(void)source.try_set_value(conflux::work::root::Success<Response>{Response::text("queued")});
			return std::move(task);
		});
	CHECK(&ctx_added == &app);
	CHECK(chttp::router(app).has_context_routes());

	auto infos = chttp::route_infos(app);
	REQUIRE(infos.size() == 2);
	CHECK(infos[0].method == "REPORT");
	CHECK(infos[0].path_pattern == "/reports/{id}");
	REQUIRE(infos[0].path_params.size() == 1);
	CHECK(infos[0].path_params[0] == "id");
	CHECK(infos[1].method == "POST");
	CHECK(infos[1].path_pattern == "/jobs/{id}");
	REQUIRE(infos[1].path_params.size() == 1);
	CHECK(infos[1].path_params[0] == "id");

	auto routes = app.routes();
	REQUIRE(routes.size() == 2);
	CHECK(routes[0].path == "/reports/{id}");
	CHECK(routes[0].handler_kind == "app");
	CHECK(routes[1].path == "/jobs/{id}");
	CHECK(routes[1].handler_kind == "context");
}

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
		router.get("/", [](Request const &) {
			return Response::html("<html><body><h1>Hello from conflux!</h1></body></html>");
		});
		router.get("/hello/{name}", [](Request const &req) {
			return Response::html(std::format("<html><body><h1>Hello, {}!</h1></body></html>", req.params["name"]));
		});
		router.get("/api/ping", [](Request const &) { return Response::json(R"({"status":"ok"})"); });
		// Pools captured by value so their lifetime ties to the router/server.
		// Avoids atexit race: a function-local static here would destruct
		// before the test server registry destructs (LIFO), while the ring
		// std::thread can still be enqueueing into the pool.
		auto defer_ok_pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 1, .max_inject_queue = 16});
		auto defer_full_pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 1});
		defer_full_pool->stop();
		router.get("/api/defer-ok", [defer_ok_pool](Request const &) {
			return conflux::http::defer(defer_ok_pool, [] { return Response::json(R"({"defer":"ok"})"); });
		});
		router.get("/api/defer-full", [defer_full_pool](Request const &) {
			return conflux::http::defer(defer_full_pool, [] { return Response::json(R"({"defer":"unreachable"})"); });
		});
		router.get("/api/task-ping", [](Request const &) -> conflux::work::root::Task<Response> {
			auto [task, source] = conflux::work::root::make_task_source<Response>();
			(void)source.try_set_value(conflux::work::root::Success<Response>{Response::json(R"({"task":"ok"})")});
			return std::move(task);
		});
		router.get("/api/echo-header", [](Request const &req) {
			auto v = req.headers["x-test-header"];
			if (v.empty()) {
				return Response::not_found("x-test-header");
			}
			return Response::text(std::string{v});
		});
		router.post("/api/echo-body", [](Request const &req) { return Response::text(req.body); });
		router.post("/api/echo-json", [](Request const &req) { return Response::json(req.body); });
		router.get("/api/echo-query", [](Request const &req) {
			auto v = req.query["key"];
			if (v.empty()) {
				return Response::not_found("key");
			}
			return Response::text(std::string{v});
		});
		router.post("/api/echo-form", [](Request const &req) {
			auto v = req.form["field"];
			if (v.empty()) {
				return Response::not_found("field");
			}
			return Response::text(std::string{v});
		});
		router.post("/api/multipart-field", [](Request const &req) {
			auto v = req.form["field"];
			if (v.empty()) {
				return Response::not_found("field");
			}
			return Response::text(std::string{v});
		});
		router.post("/api/multipart-file", [](RequestView const &req) {
			if (req.files.empty()) {
				return Response::not_found("file");
			}
			auto const &f = req.files[0];
			return Response::json(
				std::format(
					R"({{"name":"{}","filename":"{}","content_type":"{}","size":{}}})",
					f.name,
					f.filename,
					f.content_type,
					f.data.size()));
		});
		router.post("/api/multipart-counts", [](RequestView const &req) {
			return Response::json(
				std::format(R"({{"fields":{},"files":{}}})", req.form.values("field").size(), req.files.size()));
		});
		router.add_context(
			"POST",
			"/api/multipart-file-async",
			[](RequestView req, chttp::RequestContext const &) -> conflux::work::root::Task<Response> {
				auto [gate, source] = conflux::work::root::make_task_source<int>();
				std::thread([source = std::move(source)] mutable {
					std::this_thread::sleep_for(std::chrono::milliseconds{10});
					(void)source.try_set_value(conflux::work::root::Success<int>{0});
				}).detach();
				(void)co_await std::move(gate);
				if (req.files.empty()) {
					co_return Response::not_found("file");
				}
				auto const &f = req.files[0];
				co_return Response::json(
					std::format(
						R"({{"name":"{}","filename":"{}","content_type":"{}","data":"{}"}})",
						f.name,
						f.filename,
						f.content_type,
						f.data));
			});
		router.get("/api/with-header", [](Request const &) {
			auto r = Response::text("ok");
			r.headers["X-Custom"] = "hello";
			r.headers["X-Another"] = "world";
			return r;
		});
		router.get("/api/redirect-302", [](Request const &) { return Response::redirect("/api/ping"); });
		router.get("/api/redirect-301", [](Request const &) { return Response::redirect("/api/ping", 301); });
		// PUT / PATCH / DELETE / OPTIONS routes.
		router.put("/api/resource/{id}", [](Request const &req) {
			return Response::json(std::format(R"({{"method":"PUT","id":"{}"}})", req.params["id"]));
		});
		router.patch("/api/resource/{id}", [](Request const &req) {
			return Response::json(std::format(R"({{"method":"PATCH","id":"{}"}})", req.params["id"]));
		});
		router.del("/api/resource/{id}", [](Request const &req) {
			return Response::json(std::format(R"({{"method":"DELETE","id":"{}"}})", req.params["id"]));
		});
		router.options("/api/resource", [](Request const &) {
			auto r = Response::text("");
			r.status = 204;
			r.status_text = "No Content";
			r.headers["Allow"] = "GET, POST, PUT, PATCH, DELETE, OPTIONS";
			return r;
		});
		// Route group: /api/v2/* with a version header middleware.
		router.group("/api/v2", [](Router::Group &g) {
			g.use([](Request const &req, Router::Handler const &next) {
				auto resp = next(req);
				resp.headers["X-Api-Version"] = "2";
				return resp;
			});
			g.get("/status", [](Request const &) { return Response::json(R"({"v":"2","status":"ok"})"); });
			g.get("/item/{id}", [](Request const &req) {
				return Response::json(std::format(R"({{"id":"{}"}})", req.params["id"]));
			});
		});
		// Route outside the group — must NOT have X-Api-Version header.
		router.get("/api/v1/status", [](Request const &) { return Response::json(R"({"v":"1","status":"ok"})"); });
		// Cookie echo: returns value of named cookie.
		router.get("/api/echo-cookie", [](Request const &req) {
			auto v = req.cookies["name"];
			if (v.empty()) {
				return Response::not_found("name");
			}
			return Response::text(std::string{v});
		});
		// Set-cookie: sets two cookies on the response.
		router.get("/api/set-cookie", [](Request const &) {
			auto r = Response::text("ok");
			r.set_cookie("session", "abc123", "Path=/; HttpOnly");
			r.set_cookie("theme", "dark");
			return r;
		});
		// SSE endpoint: streams 3 events then closes.
		router.sse("/events", [](Request const &, std::shared_ptr<conflux::http::SseChannel> const &ch) {
			auto _ = ch->send("data: event1\n\n");
			CONFLUX_DISCARD(ch->send("data: event2\n\n"));
			CONFLUX_DISCARD(ch->send("data: event3\n\n"));
			ch->close();
		});
		// Named-param SSE endpoint.
		router.sse("/events/{name}", [](Request const &req, std::shared_ptr<conflux::http::SseChannel> const &ch) {
			auto _ = ch->send(std::format("data: hello {}\n\n", req.params["name"]));
			ch->close();
		});

		// Create server on heap so port() can be queried from this std::thread
		// while run() blocks on the worker std::thread.
		g_test_port = test_servers().start(cfg, std::move(router));
	});
}
void ensure_redirect_follow_servers() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Config cfg = mw_config();

		Router target;
		target.get("/echo-headers", [](Request const &req) {
			return Response::text(
				std::format(
					"auth={}\ncookie={}\nproxy-authorization={}\nhost={}",
					std::string{req.headers["authorization"]},
					std::string{req.headers["cookie"]},
					std::string{req.headers["proxy-authorization"]},
					std::string{req.headers["host"]}));
		});
		g_redirect_follow_target_port = test_servers().start(cfg, std::move(target));

		Router source;
		source.get("/echo-headers", [](Request const &req) {
			return Response::text(
				std::format(
					"auth={}\ncookie={}\nproxy-authorization={}\nhost={}",
					std::string{req.headers["authorization"]},
					std::string{req.headers["cookie"]},
					std::string{req.headers["proxy-authorization"]},
					std::string{req.headers["host"]}));
		});
		source.get("/same", [](Request const &) { return Response::redirect("/echo-headers"); });
		source.get("/cross", [](Request const &) {
			return Response::redirect(std::format("http://127.0.0.1:{}/echo-headers", g_redirect_follow_target_port));
		});
		source.get("/loop", [](Request const &) { return Response::redirect("/loop"); });
		source.get("/async-start", [](Request const &) { return Response::redirect("/async-final"); });
		source.get("/async-final", [](Request const &) { return Response::text("async-ok"); });
		g_redirect_follow_source_port = test_servers().start(cfg, std::move(source));

		Router front;
		auto popts = ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = g_redirect_follow_source_port,
		};
		front.get_context(
			"/async-follow",
			[popts](RequestView const &, chttp::RequestContext const &ctx) -> conflux::work::root::Task<Response> {
				HttpClient client{};
				auto result = co_await async_send(
					client,
					ctx.ring,
					chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/async-start", popts.upstream_port))
						.follow_redirects(2));
				if (!result) {
					co_return Response::bad_gateway(
						std::format(
							"redirect follow failed: {} ({})",
							result.error().message,
							static_cast<int>(result.error().kind)));
				}
				co_return Response::text(std::move(result->body));
			});
		g_redirect_follow_async_port = test_servers().start(cfg, std::move(front));
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
// Connect and read an SSE stream until the server closes the connection.
// Returns the full response (headers + all event frames).
std::string http_get_sse(
	std::string_view path) {
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
		Router router;
		router.use(compress_middleware());
		// Large body (>256 bytes) so min_body_size is exceeded.
		router.get("/big", [](Request const &) { return Response::html(std::string(512, 'A')); });
		// Small body (<256 bytes).
		router.get("/small", [](Request const &) { return Response::html("hi"); });
		// Non-compressible MIME type.
		router.get("/bin", [](Request const &) {
			Response r;
			r.status = 200;
			r.status_text = "OK";
			r.content_type = "application/octet-stream";
			r.set_text_body(std::string(512, '\x00'));
			return r;
		});
		g_compress_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_cors_compress_port = 0;
void ensure_cors_compress_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cors_middleware({.allowed_origins = {"https://test.example"}}));
		router.use(compress_middleware());
		router.get("/big", [](Request const &) { return Response::html(std::string(512, 'A')); });
		g_cors_compress_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// security_headers_middleware test server
// ---------------------------------------------------------------------------

std::uint16_t g_security_port = 0;
void ensure_security_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		SecurityOptions sopts{};
		sopts.hsts_only_on_tls = false;
		Router router;
		router.use(security_headers_middleware(sopts));
		router.get("/", [](Request const &) { return Response::text("ok"); });
		g_security_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// cors_middleware test server
// ---------------------------------------------------------------------------

std::uint16_t g_cors_port = 0;
void ensure_cors_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cors_middleware({.allowed_origins = {"https://test.example"}}));
		router.get("/api", [](Request const &) { return Response::json(R"({"ok":true})"); });
		router.get("/vary", [](Request const &) {
			auto r = Response::text("vary");
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
		Router router;
		router.use(cors_middleware({
			.allowed_origins = {"*"},
			.expose_headers = {"X-Custom-Header", "X-Request-Id"},
			.allow_credentials = true,
		}));
		router.get("/api", [](Request const &) { return Response::json(R"({"ok":true})"); });
		g_cors_cred_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_cors_wildcard_port = 0;
void ensure_cors_wildcard_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(cors_middleware()); // default: allowed_origins={"*"}, no credentials
		router.get("/api", [](Request const &) { return Response::json(R"({"ok":true})"); });
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
		Router router;
		router.use(basic_auth_middleware(
			[](std::string_view u, std::string_view p) { return u == "testuser" && p == "testpass"; }));
		router.get("/protected", [](Request const &) { return Response::text("secret"); });
		g_auth_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_bearer_port = 0;
void ensure_bearer_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(bearer_auth_middleware([](std::string_view token) { return token == "valid-token-123"; }));
		router.get("/protected", [](Request const &) { return Response::text("secret"); });
		g_bearer_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// rate_limit_middleware test server (2 req per 60s window)
// ---------------------------------------------------------------------------

std::uint16_t g_rate_port = 0;
std::uint16_t g_rate_zero_clients_port = 0;
void ensure_rate_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(rate_limit_middleware({.requests = 2, .window = std::chrono::seconds{60}}));
		router.get("/", [](Request const &) { return Response::text("ok"); });
		g_rate_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_rate_burst_port = 0;
void ensure_rate_burst_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// 1 base + 2 burst = 3 total capacity
		router.use(rate_limit_middleware({.requests = 1, .window = std::chrono::seconds{60}, .burst = 2}));
		router.get("/", [](Request const &) { return Response::text("ok"); });
		g_rate_burst_port = start_mw_server(mw_config(), std::move(router));
	});
}
void ensure_rate_zero_clients_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(rate_limit_middleware({.requests = 1, .window = std::chrono::seconds{60}, .max_clients = 0}));
		router.get("/", [](Request const &) { return Response::text("ok"); });
		g_rate_zero_clients_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// forwarded_middleware helpers
// ---------------------------------------------------------------------------

std::uint16_t g_fwd_port = 0;
std::uint16_t g_fwd_strict_empty_port = 0;
std::uint16_t g_fwd_lax_empty_port = 0;
void ensure_forwarded_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Trust only 127.0.0.1/32.
		router.use(forwarded_middleware({.trusted_proxies = {"127.0.0.1/32"}}));
		// Echo the remote_addr so tests can inspect it.
		router.get("/addr", [](Request const &req) { return Response::text(req.remote_addr); });
		g_fwd_port = start_mw_server(mw_config(), std::move(router));
	});
}
void ensure_forwarded_strict_empty_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Default strict_mode=true, empty trusted_proxies → no peer is trusted.
		router.use(forwarded_middleware({}));
		router.get("/addr", [](Request const &req) { return Response::text(req.remote_addr); });
		g_fwd_strict_empty_port = start_mw_server(mw_config(), std::move(router));
	});
}
void ensure_forwarded_lax_empty_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Legacy trust-all-on-empty behaviour.
		router.use(forwarded_middleware({.trusted_proxies = {}, .strict_mode = false}));
		router.get("/addr", [](Request const &req) { return Response::text(req.remote_addr); });
		g_fwd_lax_empty_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// request_id_middleware helpers
// ---------------------------------------------------------------------------

std::uint16_t g_rid_port = 0;
void ensure_rid_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(request_id_middleware());
		// Echo the request ID header back in the body so tests can inspect it.
		router.get("/", [](Request const &req) { return Response::text(std::string{req.headers["x-request-id"]}); });
		g_rid_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// ip_filter_middleware helpers
// ---------------------------------------------------------------------------

std::uint16_t g_ipallow_port = 0;
std::uint16_t g_ipblock_port = 0;
void ensure_ipallow_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Allow only loopback.
		router.use(ip_filter_middleware({
			.mode = IpFilterMode::allowlist,
			.cidrs = {"127.0.0.0/8"},
		}));
		router.get("/", [](Request const &) { return Response::text("ok"); });
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
		router.get("/", [](Request const &) { return Response::text("ok"); });
		g_ipblock_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_ipallow_block_port = 0;
void ensure_ipallow_block_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Allowlist that does NOT include loopback → should block us.
		router.use(ip_filter_middleware({
			.mode = IpFilterMode::allowlist,
			.cidrs = {"192.168.0.0/24"},
		}));
		router.get("/", [](Request const &) { return Response::text("ok"); });
		g_ipallow_block_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_ipblock_pass_port = 0;
void ensure_ipblock_pass_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		// Blocklist that does NOT include loopback → should pass us through.
		router.use(ip_filter_middleware({
			.mode = IpFilterMode::blocklist,
			.cidrs = {"10.0.0.0/8"},
		}));
		router.get("/", [](Request const &) { return Response::text("ok"); });
		g_ipblock_pass_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// cache_control_middleware helpers
// ---------------------------------------------------------------------------

std::uint16_t g_cache_port = 0;
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
		router.get("/image", [](Request const &) {
			Response r;
			r.content_type = "image/png";
			r.set_text_body("img");
			return r;
		});
		router.get("/css", [](Request const &) {
			Response r;
			r.content_type = "text/css";
			r.set_text_body("body{}");
			return r;
		});
		router.get("/api", [](Request const &) { return Response::json(R"({})"); });
		router.get("/html", [](Request const &) { return Response::html("<p>hi</p>"); });
		router.get("/custom", [](Request const &) {
			auto r = Response::text("x");
			r.headers["Cache-Control"] = "max-age=999";
			return r;
		});
		g_cache_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// trailing_slash_middleware helpers
// ---------------------------------------------------------------------------

std::uint16_t g_ts_remove_port = 0;
std::uint16_t g_ts_add_port = 0;
void ensure_ts_remove_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(trailing_slash_middleware()); // default: remove
		router.get("/foo", [](Request const &) { return Response::text("foo"); });
		g_ts_remove_port = start_mw_server(mw_config(), std::move(router));
	});
}
void ensure_ts_add_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(trailing_slash_middleware({.mode = TrailingSlashMode::add}));
		router.get("/bar/", [](Request const &) { return Response::text("bar"); });
		g_ts_add_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_ts_308_port = 0;
void ensure_ts_308_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(trailing_slash_middleware({.redirect_status = 308}));
		router.get("/foo", [](Request const &) { return Response::text("foo"); });
		g_ts_308_port = start_mw_server(mw_config(), std::move(router));
	});
}
std::uint16_t g_ts_307_port = 0;
void ensure_ts_307_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(trailing_slash_middleware({.redirect_status = 307}));
		router.get("/foo", [](Request const &) { return Response::text("foo"); });
		g_ts_307_port = start_mw_server(mw_config(), std::move(router));
	});
}
// POST to an explicit port with extra headers (for CSRF token etc.).
std::string http_post_on_full(
	std::uint16_t port,
	std::string_view path,
	std::string_view content_type,
	std::string_view body,
	std::string_view extra_headers) {
	return conflux::tests::http_post_on(port, path, content_type, body, extra_headers);
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
Document require_json_text(
	std::string_view text) {
	auto doc = conflux::json::parse_copy(std::string{text});
	REQUIRE(doc.has_value());
	return std::move(*doc);
}
NodeRef require_json_pointer(
	Document const &doc,
	std::string_view pointer) {
	auto node = doc.root().at_pointer(pointer);
	REQUIRE(node.has_value());
	return *node;
}
void check_json_string_at(
	Document const &doc,
	std::string_view pointer,
	std::string_view expected) {
	auto node = require_json_pointer(doc, pointer);
	auto value = node.as_string();
	REQUIRE(value.has_value());
	CHECK(*value == expected);
}
void check_json_u64_at(
	Document const &doc,
	std::string_view pointer,
	std::uint64_t expected) {
	auto node = require_json_pointer(doc, pointer);
	auto value = node.as_u64();
	REQUIRE(value.has_value());
	CHECK(*value == expected);
}
void check_json_absent_at(
	Document const &doc,
	std::string_view pointer) {
	CHECK_FALSE(doc.root().at_pointer(pointer).has_value());
}
// Extract the value of a named cookie from a Set-Cookie header list.
// Looks for "Set-Cookie: <name>=<value>; ..." lines.
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
// ---------------------------------------------------------------------------
// redirect_middleware test server
// ---------------------------------------------------------------------------

std::uint16_t g_redirect_port = 0;
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
		router.get("/new", [](Request const &) { return Response::text("new"); });
		router.get("/api/v2/users", [](Request const &) { return Response::text("v2-users"); });
		g_redirect_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// csrf_middleware test server
// ---------------------------------------------------------------------------

std::uint16_t g_csrf_port = 0;
void ensure_csrf_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(csrf_middleware());
		router.get("/page", [](Request const &) { return Response::html("<form>"); });
		router.post("/submit", [](Request const &) { return Response::text("ok"); });
		g_csrf_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// etag_middleware test server
// ---------------------------------------------------------------------------

std::uint16_t g_etag_port = 0;
void ensure_etag_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(etag_middleware());
		router.get("/content", [](Request const &) { return Response::text("hello world"); });
		router.get("/empty", [](Request const &) { return Response::text(""); });
		g_etag_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// response_cache_middleware test server
// ---------------------------------------------------------------------------

std::atomic<int> g_resp_cache_count{0};
std::uint16_t g_resp_cache_port = 0;
void ensure_resp_cache_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(response_cache_middleware({
			.max_entries = 16,
			.default_ttl = std::chrono::seconds{60},
		}));
		router.get("/counted", [](Request const &) {
			int n = ++g_resp_cache_count;
			return Response::text(std::format("visit {}", n));
		});
		router.post("/counted", [](Request const &) {
			int n = ++g_resp_cache_count;
			return Response::text(std::format("post {}", n));
		});
		router.get("/no-store", [](Request const &) {
			auto r = Response::text("uncacheable");
			r.headers["Cache-Control"] = "no-store";
			return r;
		});
		router.get("/vary", [](Request const &req) {
			int n = ++g_resp_cache_count;
			auto r = Response::text(std::format("{} enc={}", n, req.headers["accept-encoding"]));
			r.headers["Vary"] = "Accept-Encoding";
			return r;
		});
		router.get("/vary-star", [](Request const &) {
			int n = ++g_resp_cache_count;
			auto r = Response::text(std::format("star {}", n));
			r.headers["Vary"] = "*";
			return r;
		});
		router.get("/private", [](Request const &) {
			int n = ++g_resp_cache_count;
			auto r = Response::text(std::format("priv {}", n));
			r.headers["Cache-Control"] = "private";
			return r;
		});
		router.get("/max-age-zero", [](Request const &) {
			int n = ++g_resp_cache_count;
			auto r = Response::text(std::format("zero {}", n));
			r.headers["Cache-Control"] = "max-age=0";
			return r;
		});
		router.get("/no-cache", [](Request const &) {
			int n = ++g_resp_cache_count;
			auto r = Response::text(std::format("nocache {}", n));
			r.headers["Cache-Control"] = "no-cache";
			return r;
		});
		router.get("/cache-control-substrings", [](Request const &) {
			int n = ++g_resp_cache_count;
			auto r = Response::text(std::format("substrings {}", n));
			r.headers["Cache-Control"] = "no-storehouse, privateer, no-cacheable, s-maxage=0";
			return r;
		});
		router.get("/set-cookie-resp", [](Request const &) {
			int n = ++g_resp_cache_count;
			auto r = Response::text(std::format("cookie {}", n));
			r.set_cookie("sid", "abc");
			return r;
		});
		router.get("/not-found-resp", [](Request const &) {
			++g_resp_cache_count;
			return Response::not_found("/not-found-resp");
		});
		router.get("/query", [](Request const &req) {
			int n = ++g_resp_cache_count;
			return Response::text(std::format("{} value={}", n, req.query["value"]));
		});
		g_resp_cache_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// structured_log_middleware test server
// ---------------------------------------------------------------------------

std::uint16_t g_slog_port = 0;
char g_slog_path[64]{};
void ensure_slog_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		std::strcpy(g_slog_path, "/tmp/conflux_slog_XXXXXX");
		int const tmp = ::mkstemp(g_slog_path);
		::close(tmp);

		Router router;
		router.use(structured_log_middleware({.log_file = g_slog_path, .app_name = "test"}));
		router.get("/ping", [](Request const &) { return Response::text("pong"); });
		g_slog_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// tracing_middleware test server
// ---------------------------------------------------------------------------

std::uint16_t g_trace_port = 0;
void ensure_trace_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router router;
		router.use(tracing_middleware({.propagate_in_response = true}));
		// Echo the injected traceparent header so tests can verify it.
		router.get("/", [](Request const &req) { return Response::text(std::string{req.headers["traceparent"]}); });
		g_trace_port = start_mw_server(mw_config(), std::move(router));
	});
}
// ---------------------------------------------------------------------------
// VHostRouter test server
// ---------------------------------------------------------------------------

std::uint16_t g_vhost_port = 0;
std::uint16_t g_vhost_direct_port = 0;
std::uint16_t g_proxy_port = 0;
std::shared_ptr<ScopedTestServer> g_proxy_upstream;
std::shared_ptr<ScopedTestServer> g_proxy_front;
void ensure_vhost_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router api_router;
		api_router.get("/status", [](Request const &) { return Response::text("api"); });

		Router web_router;
		web_router.get("/status", [](Request const &) { return Response::text("web"); });

		Router def_router;
		def_router.get("/status", [](Request const &) { return Response::text("default"); });

		auto vhr = std::make_shared<VHostRouter>();
		vhr->add("api.example.com", std::move(api_router));
		vhr->add("web.example.com", std::move(web_router));
		vhr->set_default(std::move(def_router));

		Router main;
		main.use([vhr](Request const &req, Router::Handler const &) { return vhr->dispatch(req); });
		g_vhost_port = start_mw_server(mw_config(), std::move(main));
	});
}
void ensure_vhost_direct_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Router api_router;
		api_router.get("/status", [](Request const &) { return Response::text("api-direct"); });

		Router def_router;
		def_router.get("/status", [](Request const &) { return Response::text("default-direct"); });

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
		upstream.get("/ping", [](Request const &) {
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
			auto resp = Response::text("proxied-ok");
			resp.headers["X-Upstream"] = "yes";
			return resp;
		});
		g_proxy_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));

		Router front;
		auto popts = ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = g_proxy_upstream->port(),
			.path_prefix = "/proxy",
		};
		front.add_context(
			"GET",
			"/proxy/ping",
			[popts = std::move(popts)](RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<Response> { co_return co_await async_proxy(req, popts, ctx.ring); });
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
// ---------------------------------------------------------------------------
// Router — path matching
// ---------------------------------------------------------------------------

TEST_CASE(
	"router dispatch preserves generic route priority before literal index hits") {
	Router router;
	router.get("/{id}", [](RequestView const &req) {
		return Response::text(std::string{"generic:"} + std::string{req.params["id"]});
	});
	router.get("/health", [](RequestView const &) { return Response::text("literal"); });

	Request req;
	req.method = "GET";
	req.path = "/health";
	req.version = "HTTP/1.1";

	CHECK(router.dispatch(req).text_body() == "generic:health");
}

TEST_CASE(
	"router dispatch uses method-scoped literal lookup") {
	Router router;
	router.post("/health", [](RequestView const &) { return Response::text("post"); });
	router.get("/health", [](RequestView const &) { return Response::text("get"); });

	Request req;
	req.method = "GET";
	req.path = "/health";
	req.version = "HTTP/1.1";

	CHECK(router.dispatch(req).text_body() == "get");
}

TEST_CASE(
	"GET /api/ping returns JSON") {
	auto resp = http_get("/api/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("application/json") != std::string::npos);
	REQUIRE(resp.find(R"("status":"ok")") != std::string::npos);
}
TEST_CASE(
	"GET /api/defer-ok returns deferred payload") {
	auto resp = http_get("/api/defer-ok");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find(R"("defer":"ok")") != std::string::npos);
}
TEST_CASE(
	"GET /api/defer-full returns queue-full error") {
	auto resp = http_get("/api/defer-full");
	REQUIRE(resp.starts_with("HTTP/1.1 500 Internal Server Error"));
	REQUIRE(resp.find("offload queue full") != std::string::npos);
}
TEST_CASE(
	"GET /api/task-ping returns root task payload") {
	auto resp = http_get("/api/task-ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find(R"("task":"ok")") != std::string::npos);
}
TEST_CASE(
	"http client: GET /api/ping returns parsed response") {
	ensure_server();
	auto response =
		HttpClient{}.blocking_send(chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/ping", g_test_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body == R"({"status":"ok"})");
}
TEST_CASE(
	"http client: GET /api/ping returns parsed response (blocking_send)") {
	ensure_server();
	HttpClient client{};
	auto response =
		client.blocking_send(chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/ping", g_test_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body == R"({"status":"ok"})");
}
TEST_CASE(
	"http client: convenience client sends headers and parses response headers") {
	ensure_server();
	HttpClient client{};
	HttpFields headers{true};
	headers["X-Test-Header"] = "client-header";

	auto response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/echo-header", g_test_port)).headers(headers));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body == "client-header");

	auto with_headers = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/with-header", g_test_port)));
	REQUIRE(with_headers);
	CHECK(with_headers->head.headers["x-custom"] == "hello");
	CHECK(with_headers->head.headers["x-another"] == "world");
}
TEST_CASE(
	"http client: request headers override default headers once") {
	ensure_server();
	HttpClientOptions opts{};
	opts.default_headers["X-Test-Header"] = "default";
	HttpClient client{opts};

	auto default_response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/echo-header", g_test_port)));
	REQUIRE(default_response);
	CHECK(default_response->head.status == 200);
	CHECK(default_response->body == "default");

	auto override_response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/echo-header", g_test_port))
			.header("x-test-header", "override"));
	REQUIRE(override_response);
	CHECK(override_response->head.status == 200);
	CHECK(override_response->body == "override");
}
TEST_CASE(
	"http client: convenience client POST sends body and content type") {
	ensure_server();
	HttpClient client{};

	auto response = client.blocking_send(
		chttp::ClientRequest::post(std::format("http://127.0.0.1:{}/api/echo-json", g_test_port))
			.content_type("application/json")
			.body(R"({"from":"client"})"));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body == R"({"from":"client"})");
}
TEST_CASE(
	"http client: PUT sends body and receives response") {
	ensure_server();
	HttpClient client{};

	auto response = client.blocking_send(
		chttp::ClientRequest::put(std::format("http://127.0.0.1:{}/api/resource/42", g_test_port))
			.content_type("application/json")
			.body(R"({"x":1})"));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("PUT") != std::string::npos);
	CHECK(response->body.find("42") != std::string::npos);
}
TEST_CASE(
	"http client: PATCH sends body and receives response") {
	ensure_server();
	HttpClient client{};

	auto response = client.blocking_send(
		chttp::ClientRequest::patch(std::format("http://127.0.0.1:{}/api/resource/7", g_test_port))
			.content_type("application/json")
			.body(R"({"delta":1})"));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("PATCH") != std::string::npos);
	CHECK(response->body.find("7") != std::string::npos);
}
TEST_CASE(
	"http client: DELETE returns response") {
	ensure_server();
	HttpClient client{};

	auto response = client.blocking_send(
		chttp::ClientRequest::del(std::format("http://127.0.0.1:{}/api/resource/99", g_test_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("DELETE") != std::string::npos);
	CHECK(response->body.find("99") != std::string::npos);
}
TEST_CASE(
	"http client: HEAD /api/ping returns 200 with no body") {
	ensure_server();
	HttpClient client{};

	auto response =
		client.blocking_send(chttp::ClientRequest::head(std::format("http://127.0.0.1:{}/api/ping", g_test_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body.empty());
}
TEST_CASE(
	"http client: blocking_send works without pool") {
	ensure_server();
	HttpClient client{};
	auto response =
		client.blocking_send(chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/ping", g_test_port)));
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
	auto response = client.blocking_send(chttp::ClientRequest::get("http://127.0.0.1:9/"));
	REQUIRE_FALSE(response);
	CHECK(response.error().kind == HttpErrorKind::connect);
}
TEST_CASE(
	"http client: follow_redirects follows same-origin relative redirects") {
	ensure_redirect_follow_servers();
	HttpClient client{};
	auto response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/same", g_redirect_follow_source_port))
			.header("Authorization", "Bearer secret")
			.header("Cookie", "session=abc")
			.header("Proxy-Authorization", "Basic proxy")
			.follow_redirects(2));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("auth=Bearer secret") != std::string::npos);
	CHECK(response->body.find("cookie=session=abc") != std::string::npos);
	CHECK(response->body.find("proxy-authorization=Basic proxy") == std::string::npos);
	CHECK(response->body.find(std::format("host=127.0.0.1:{}", g_redirect_follow_source_port)) != std::string::npos);
}
TEST_CASE(
	"http client: follow_redirects strips sensitive headers across host changes") {
	ensure_redirect_follow_servers();
	HttpClient client{};
	auto response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/cross", g_redirect_follow_source_port))
			.header("Authorization", "Bearer secret")
			.header("Cookie", "session=abc")
			.header("Proxy-Authorization", "Basic proxy")
			.follow_redirects(2));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("auth=") != std::string::npos);
	CHECK(response->body.find("cookie=") != std::string::npos);
	CHECK(response->body.find("proxy-authorization=") != std::string::npos);
	CHECK(response->body.find("Bearer secret") == std::string::npos);
	CHECK(response->body.find("session=abc") == std::string::npos);
	CHECK(response->body.find("Basic proxy") == std::string::npos);
	CHECK(response->body.find(std::format("host=127.0.0.1:{}", g_redirect_follow_target_port)) != std::string::npos);
}
TEST_CASE(
	"http client: follow_redirects reports redirect limit exhaustion") {
	ensure_redirect_follow_servers();
	HttpClient client{};
	auto response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/loop", g_redirect_follow_source_port))
			.follow_redirects(1));
	REQUIRE_FALSE(response);
	CHECK(response.error().kind == HttpErrorKind::redirect_limit);
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
// SSE — Server-Sent Events
// ---------------------------------------------------------------------------

TEST_CASE(
	"SSE /events returns text/event-stream") {
	auto resp = http_get_sse("/events");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Type: text/event-stream") != std::string::npos);
}
TEST_CASE(
	"SSE /events streams all 3 events") {
	auto resp = http_get_sse("/events");
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != std::string::npos);
	auto body = std::string_view{resp}.substr(body_start + 4);
	REQUIRE(body.find("data: event1") != std::string_view::npos);
	REQUIRE(body.find("data: event2") != std::string_view::npos);
	REQUIRE(body.find("data: event3") != std::string_view::npos);
}
TEST_CASE(
	"SSE /events/{name} captures param") {
	auto resp = http_get_sse("/events/alice");
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != std::string::npos);
	auto body = std::string_view{resp}.substr(body_start + 4);
	REQUIRE(body.find("data: hello alice") != std::string_view::npos);
}
// ---------------------------------------------------------------------------
// SseBroadcaster unit-level tests
// ---------------------------------------------------------------------------

TEST_CASE(
	"SseBroadcaster: subscriber_count tracks subscriptions") {
	conflux::http::SseBroadcaster bc;
	REQUIRE(bc.subscriber_count() == 0);
	auto ch1 = bc.subscribe();
	REQUIRE(bc.subscriber_count() == 1);
	auto ch2 = bc.subscribe();
	REQUIRE(bc.subscriber_count() == 2);
}
TEST_CASE(
	"SseBroadcaster: stale subscriber is evicted on broadcast") {
	conflux::http::SseBroadcaster bc;
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
	Router router;
	router.post("/upload", [](Request const &req) { return Response::text(req.body); });
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
// URL-encoded form (POST form)
// ---------------------------------------------------------------------------

TEST_CASE(
	"urlencoded form field is parsed") {
	auto resp = http_post("/api/echo-form", "application/x-www-form-urlencoded", "field=hello");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello");
}
TEST_CASE(
	"urlencoded form field is percent-decoded") {
	auto resp = http_post("/api/echo-form", "application/x-www-form-urlencoded", "field=hello%20world");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello world");
}
TEST_CASE(
	"urlencoded form with multiple fields parses target field") {
	auto resp = http_post("/api/echo-form", "application/x-www-form-urlencoded", "other=x&field=target&more=y");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
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
static std::string make_multipart_text(
	std::string_view boundary,
	std::string_view name,
	std::string_view field_value) {
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
static std::string make_multipart_file(
	std::string_view boundary,
	std::string_view name,
	std::string_view filename,
	std::string_view content_type,
	std::string_view data) {
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
static std::string make_multipart_text_and_file(
	std::string_view boundary,
	std::string_view field_name,
	std::string_view field_value,
	std::string_view file_name,
	std::string_view filename,
	std::string_view content_type,
	std::string_view data) {
	return std::format(
		"--{}\r\n"
		"Content-Disposition: form-data; name=\"{}\"\r\n"
		"\r\n"
		"{}\r\n"
		"--{}\r\n"
		"Content-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\n"
		"Content-Type: {}\r\n"
		"\r\n"
		"{}\r\n"
		"--{}--\r\n",
		boundary,
		field_name,
		field_value,
		boundary,
		file_name,
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
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello from multipart");
}
TEST_CASE(
	"multipart/form-data value with special characters is preserved") {
	auto body = make_multipart_text("bnd42", "field", "a=1&b=2 <> \"quotes\"");
	auto ct = std::string{"multipart/form-data; boundary=bnd42"};
	auto resp = http_post("/api/multipart-field", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "a=1&b=2 <> \"quotes\"");
}
TEST_CASE(
	"multipart/form-data file part populates req.files") {
	auto body = make_multipart_file("fileBnd", "upload", "hello.txt", "text/plain", "file content here");
	auto ct = std::string{"multipart/form-data; boundary=fileBnd"};
	auto resp = http_post("/api/multipart-file", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	auto json = resp.substr(hdr_end + 4);
	REQUIRE(json.find("\"name\":\"upload\"") != std::string::npos);
	REQUIRE(json.find("\"filename\":\"hello.txt\"") != std::string::npos);
	REQUIRE(json.find("\"content_type\":\"text/plain\"") != std::string::npos);
	REQUIRE(json.find("\"size\":17") != std::string::npos);
}

TEST_CASE(
	"multipart/form-data quoted semicolon filename is preserved") {
	auto body = make_multipart_file("semiBnd", "upload", "hello;semi.txt", "text/plain", "file content here");
	auto ct = std::string{"multipart/form-data; boundary=semiBnd"};
	auto resp = http_post("/api/multipart-file", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto json = extract_body(resp);
	REQUIRE(json.find("\"filename\":\"hello;semi.txt\"") != std::string::npos);
}

TEST_CASE(
	"multipart/form-data file part survives async suspension") {
	auto body = make_multipart_file("asyncBnd", "upload", "async.txt", "text/plain", "async file content");
	auto resp = http_post("/api/multipart-file-async", "multipart/form-data; boundary=asyncBnd", body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	auto json = resp.substr(hdr_end + 4);
	REQUIRE(json.find("\"name\":\"upload\"") != std::string::npos);
	REQUIRE(json.find("\"filename\":\"async.txt\"") != std::string::npos);
	REQUIRE(json.find("\"content_type\":\"text/plain\"") != std::string::npos);
	REQUIRE(json.find("\"data\":\"async file content\"") != std::string::npos);
}
TEST_CASE(
	"async request buffer cut preserves pipelined follow-up request") {
	ensure_server();
	LocalTcpClient client{g_test_port};
	client.set_recv_timeout(std::chrono::seconds{5});

	auto body = make_multipart_file("pipeBnd", "upload", "pipe.txt", "text/plain", "pipelined file");
	auto req = std::format(
		"POST /api/multipart-file-async HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Type: multipart/form-data; boundary=pipeBnd\r\n"
		"Content-Length: {}\r\n"
		"\r\n"
		"{}"
		"GET /api/ping HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Connection: close\r\n"
		"\r\n",
		body.size(),
		body);
	(void)client.send(req);
	auto combined = client.read_until_close();
	auto first_pos = combined.find("HTTP/1.1 200 OK");
	REQUIRE(first_pos != std::string::npos);
	auto second_pos = combined.find("HTTP/1.1 200 OK", first_pos + 1);
	REQUIRE(second_pos != std::string::npos);
	auto first = combined.substr(first_pos, second_pos - first_pos);
	auto second = combined.substr(second_pos);
	REQUIRE(extract_body(first).find("\"data\":\"pipelined file\"") != std::string::npos);
	REQUIRE(extract_body(second) == R"({"status":"ok"})");
}
TEST_CASE(
	"async extracted body reference survives suspension") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/async-body-ref", [](chttp::BodyText const &body) -> chttp::Task<chttp::Response> {
		auto [gate, source] = conflux::work::root::make_task_source<int>();
		std::thread([source = std::move(source)] mutable {
			std::this_thread::sleep_for(std::chrono::milliseconds{10});
			(void)source.try_set_value(conflux::work::root::Success<int>{0});
		}).detach();
		(void)co_await std::move(gate);
		co_return chttp::text(body.get());
	});
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_post_on((*server)->port(), "/async-body-ref", "text/plain", "borrowed-body");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "borrowed-body");
}
TEST_CASE(
	"async request view handler survives suspension") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/async-request-view", [](chttp::RequestView req) -> chttp::Task<chttp::Response> {
		auto [gate, source] = conflux::work::root::make_task_source<int>();
		std::thread([source = std::move(source)] mutable {
			std::this_thread::sleep_for(std::chrono::milliseconds{10});
			(void)source.try_set_value(conflux::work::root::Success<int>{0});
		}).detach();
		(void)co_await std::move(gate);
		co_return chttp::text(std::format("{}:{}", req.header("x-check"), req.body));
	});
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_request_on(
		(*server)->port(),
		"POST",
		"/async-request-view",
		"text/plain",
		"borrowed-body",
		"X-Check: alive\r\n"
		"Connection: close\r\n");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "alive:borrowed-body");
}

TEST_CASE(
	"async context route timeout returns gateway timeout and cancels handler with deadline") {
	namespace root = conflux::work::root;

	std::atomic<int> observed_reason{-1};
	Config cfg = Config::public_server();
	cfg.rings = 1;
	cfg.ring_entries = 64;
	cfg.startup_banner = false;
	Router router;
	auto timeout = std::make_shared<std::chrono::milliseconds>(25);
	router.add_context_with_timeout(
		"POST",
		"/async-timeout",
		timeout,
		[&observed_reason](RequestView const &, chttp::RequestContext const &) -> chttp::Task<chttp::Response> {
			auto source_slot = std::make_shared<std::optional<root::TaskSource<chttp::Response>>>();
			auto [task, source] = root::make_cancellable_task_source<chttp::Response>(
				[&observed_reason, source_slot](root::CancelReason reason) noexcept {
					observed_reason.store(static_cast<int>(reason), std::memory_order_release);
					if (*source_slot) {
						auto source = std::move(**source_slot);
						source_slot->reset();
						(void)source.try_set_cancelled(reason);
					}
				});
			source_slot->emplace(std::move(source));
			return std::move(task);
		});
	auto port = test_servers().start(cfg, std::move(router));
	auto resp = conflux::tests::http_post_on(port, "/async-timeout", "text/plain", "body");
	REQUIRE(resp.starts_with("HTTP/1.1 504 Gateway Timeout"));
	for (int i = 0; i != 50 && observed_reason.load(std::memory_order_acquire) == -1; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
	}
	CHECK(observed_reason.load(std::memory_order_acquire) == static_cast<int>(root::CancelReason::deadline));
}

TEST_CASE(
	"async middleware request view survives suspension") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.use(
		[](chttp::RequestView req,
		   chttp::RequestContext const &ctx,
		   chttp::AsyncNext const &next) -> chttp::Task<chttp::Response> {
			auto [gate, source] = conflux::work::root::make_task_source<int>();
			std::thread([source = std::move(source)] mutable {
				std::this_thread::sleep_for(std::chrono::milliseconds{10});
				(void)source.try_set_value(conflux::work::root::Success<int>{0});
			}).detach();
			(void)co_await std::move(gate);
			auto response = co_await next(req, ctx);
			response.headers.set("x-async-middleware-view", std::string{req.header("x-check")});
			co_return response;
		});
	app.post("/async-middleware-view", [](chttp::BodyText const &body) { return chttp::text(body.get()); });
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_request_on(
		(*server)->port(),
		"POST",
		"/async-middleware-view",
		"text/plain",
		"borrowed-body",
		"X-Check: alive\r\n"
		"Connection: close\r\n");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("x-async-middleware-view: alive") != std::string::npos);
	REQUIRE(extract_body(resp) == "borrowed-body");
}
TEST_CASE(
	"multipart/form-data parses each part exactly once") {
	auto body =
		make_multipart_text_and_file("countBnd", "field", "value", "upload", "hello.txt", "text/plain", "file content");
	auto resp = http_post("/api/multipart-counts", "multipart/form-data; boundary=countBnd", body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == R"({"fields":1,"files":1})");
}
TEST_CASE(
	"multipart/form-data delimiter text inside file content is preserved") {
	std::string const data = "before --fileBnd after";
	auto body = make_multipart_file("fileBnd", "upload", "hello.txt", "text/plain", data);
	auto ct = std::string{"multipart/form-data; boundary=fileBnd"};
	auto resp = http_post("/api/multipart-file", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	auto json = resp.substr(hdr_end + 4);
	REQUIRE(json.find(std::format("\"size\":{}", data.size())) != std::string::npos);
}
TEST_CASE(
	"multipart/form-data part header without space after colon is parsed") {
	std::string const body =
		"--bNoSpace\r\n"
		"Content-Disposition:form-data; name=\"field\"\r\n"
		"\r\n"
		"hello\r\n"
		"--bNoSpace--\r\n";
	auto resp = http_post("/api/multipart-field", "multipart/form-data; boundary=bNoSpace", body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "hello");
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
TEST_CASE(
	"percent-encoded UTF-8 in urlencoded form field is decoded correctly") {
	// こんにちは percent-encoded
	auto resp = http_post(
		"/api/echo-form",
		"application/x-www-form-urlencoded",
		"field=%E3%81%93%E3%82%93%E3%81%AB%E3%81%A1%E3%81%AF");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == "こんにちは");
}
// ---------------------------------------------------------------------------
// Response headers
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
	Router router;
	router.get("/bad-status", [](Request const &) {
		Response r = Response::text("ok");
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
	Router router;
	router.get("/path", [](Request const &) { return Response::text("ok"); });
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
	router.get("/ping", [](Request const &) { return Response::text("pong"); });

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const port = srv.port();

	// Verify it responds before shutdown.
	{
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
		std::string_view const req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
		::send(fd, req.data(), req.size(), 0);
		auto resp = read_one_response(fd);
		::close(fd);
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	}

	// Shutdown and wait for run() to return.
	srv.stop(); // if this returns, run() exited cleanly

	// Verify the listener is closed rather than merely accepting and delaying a response.
	bool refused = false;
	for (int i = 0; i < 50 && !refused; ++i) {
		int const fd2 = ::socket(AF_INET, SOCK_STREAM, 0);
		REQUIRE(fd2 >= 0);
		sockaddr_in addr2{};
		addr2.sin_family = AF_INET;
		addr2.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr2.sin_addr);
		int const conn_result = ::connect(fd2, reinterpret_cast<sockaddr *>(&addr2), sizeof(addr2));
		int const connect_errno = errno;
		::close(fd2);
		if (conn_result < 0) {
			refused = connect_errno == ECONNREFUSED || connect_errno == ECONNRESET || connect_errno == ENOENT;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(refused);
}

TEST_CASE(
	"drain closes idle keep-alive connection and reports idle close",
	"[http.lifecycle]") {
	Config cfg = mw_config();
	Router router;
	router.get("/ping", [](Request const &) { return Response::text("pong"); });

	ScopedTestServer srv{cfg, std::move(router)};
	LocalTcpClient client{srv.port()};
	(void)client.send("GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n");
	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Connection: keep-alive") != std::string::npos);

	auto report = srv.drain(DrainOptions{.deadline = std::chrono::milliseconds{2000}});
	CHECK(report.accepted_before_stop >= 1);
	CHECK(report.idle_closed >= 1);
	CHECK_FALSE(report.deadline_hit);

	char probe{};
	CHECK(client.recv(&probe, 1) == 0);
	auto metrics = srv.metrics();
	CHECK(metrics.pressure.drain_started >= 1);
}

TEST_CASE(
	"drain stops new accepts",
	"[http.lifecycle]") {
	Config cfg = mw_config();
	Router router;
	router.get("/ping", [](Request const &) { return Response::text("pong"); });

	ScopedTestServer srv{cfg, std::move(router)};
	auto const port = srv.port();
	auto before = http_get_on(port, "/ping", "Connection: close\r\n");
	REQUIRE(before.starts_with("HTTP/1.1 200 OK"));

	auto report = srv.drain(DrainOptions{.deadline = std::chrono::milliseconds{2000}});
	CHECK_FALSE(report.deadline_hit);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	auto const rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	auto const connect_errno = errno;
	::close(fd);
	REQUIRE(rc < 0);
	bool const refused_or_reset =
		connect_errno == ECONNREFUSED || connect_errno == ECONNRESET || connect_errno == ENOENT;
	CHECK(refused_or_reset);
}

TEST_CASE(
	"drain lets in-flight response finish",
	"[http.lifecycle]") {
	Config cfg = mw_config();
	Router router;
	router.get("/large", [](Request const &) { return Response::text(std::string(512 * 1024, 'x')); });

	ScopedTestServer srv{cfg, std::move(router)};
	LocalTcpClient client{srv.port()};
	(void)client.send("GET /large HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	auto headers = client.read_headers();
	REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));

	auto report = srv.drain(DrainOptions{.deadline = std::chrono::milliseconds{5000}, .finish_requests = true});
	CHECK_FALSE(report.deadline_hit);
	auto resp = headers + client.read_until_close();
	CHECK(resp.starts_with("HTTP/1.1 200 OK"));
	CHECK(resp.find("Content-Length: 524288") != std::string::npos);
	auto const header_end = resp.find("\r\n\r\n");
	REQUIRE(header_end != std::string::npos);
	auto const body = std::string_view{resp}.substr(header_end + 4);
	CHECK(body.size() == 512UZ * 1024UZ);
	CHECK(std::ranges::all_of(body, [](char c) { return c == 'x'; }));
	CHECK(report.requests_finished >= 1);
}

TEST_CASE(
	"drain deadline reports hit when idle close is disabled",
	"[http.lifecycle]") {
	Config cfg = mw_config();
	Router router;
	router.get("/ping", [](Request const &) { return Response::text("pong"); });

	ScopedTestServer srv{cfg, std::move(router)};
	LocalTcpClient client{srv.port()};
	(void)client.send("GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n");
	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));

	auto report = srv.drain(DrainOptions{.deadline = std::chrono::milliseconds{1}, .close_idle = false});
	CHECK(report.deadline_hit);
	auto metrics = srv.metrics();
	CHECK(metrics.pressure.drain_started >= 1);
}

#include "http_e2e_middleware.cxx"
#include "http_e2e_observability.cxx"
