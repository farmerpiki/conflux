#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.async_client;
import conflux.net.config;
import conflux.net.http.client;
import conflux.net.proxy;
import conflux.net.router;
import conflux.tests.support;
import conflux.work;

using namespace conflux::tests;
namespace {
namespace chttp = conflux::http;
using conflux::http::HttpClient;

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

std::uint16_t g_proxy_port = 0;
std::shared_ptr<ScopedTestServer> g_proxy_upstream;
std::shared_ptr<ScopedTestServer> g_proxy_front;

void ensure_proxy_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();

		conflux::http::Router upstream;
		upstream.get("/ping", [](conflux::http::OwnedRequest const &) {
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
			auto resp = conflux::http::Response::text("proxied-ok");
			resp.headers["X-Upstream"] = "yes";
			return resp;
		});
		g_proxy_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));

		conflux::http::Router front;
		auto popts = conflux::http::ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = g_proxy_upstream->port(),
			.path_prefix = "/proxy",
		};
		front.add_context(
			"GET",
			"/proxy/ping",
			[popts = std::move(popts)](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await conflux::http::async_proxy(req, popts, ctx.ring);
			});
		g_proxy_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
		g_proxy_port = g_proxy_front->port();
	});
}

std::uint16_t g_redirect_follow_source_port = 0;
std::uint16_t g_redirect_follow_async_port = 0;

void ensure_redirect_follow_servers() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();

		conflux::http::Router source;
		source.get("/async-start", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::redirect("/async-final");
		});
		source.get("/async-final", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("async-ok");
		});
		g_redirect_follow_source_port = test_servers().start(cfg, std::move(source));

		conflux::http::Router front;
		auto popts = conflux::http::ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = g_redirect_follow_source_port,
		};
		front.get_context(
			"/async-follow",
			[popts](conflux::http::RequestView const &, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				HttpClient client{};
				auto result = co_await async_send(
					client,
					ctx.ring,
					chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/async-start", popts.upstream_port))
						.follow_redirects(2));
				if (!result) {
					co_return conflux::http::Response::bad_gateway(
						std::format(
							"redirect follow failed: {} ({})",
							result.error().message,
							static_cast<int>(result.error().kind)));
				}
				co_return conflux::http::Response::text(std::move(result->body));
			});
		g_redirect_follow_async_port = test_servers().start(cfg, std::move(front));
	});
}

} // namespace

TEST_CASE(
	"proxy: work-pool proxy handler forwards upstream response off-ring") {
	ensure_proxy_server();
	auto resp = http_get_on(g_proxy_port, "/proxy/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("X-Upstream: yes\r\n") != std::string::npos);
	REQUIRE(extract_body(resp) == "proxied-ok");
}

TEST_CASE(
	"http client async: async_send follows relative redirects") {
	ensure_redirect_follow_servers();
	auto resp = http_get_on(g_redirect_follow_async_port, "/async-follow");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "async-ok");
}

TEST_CASE(
	"proxy: preserve_host=true forwards original Host header") {
	static std::shared_ptr<ScopedTestServer> s_upstream;
	static std::shared_ptr<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router upstream;
		upstream.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["host"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		conflux::http::Router front;
		auto popts = conflux::http::ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = s_upstream->port(),
			.preserve_host = true,
		};
		front.add_context(
			"GET",
			"/echo",
			[popts = std::move(popts)](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await conflux::http::async_proxy(req, popts, ctx.ring);
			});
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	auto resp = http_get_on(s_front->port(), "/echo");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "localhost");
}

TEST_CASE(
	"proxy: preserve_host=true with port in Host header still connects to upstream") {
	static std::shared_ptr<ScopedTestServer> s_upstream;
	static std::shared_ptr<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router upstream;
		upstream.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["host"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		conflux::http::Router front;
		auto popts = conflux::http::ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = s_upstream->port(),
			.preserve_host = true,
		};
		front.add_context(
			"GET",
			"/echo",
			[popts = std::move(popts)](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await conflux::http::async_proxy(req, popts, ctx.ring);
			});
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	auto resp = http_get_on_host(s_front->port(), "localhost:9999", "/echo");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "localhost:9999");
}

TEST_CASE(
	"proxy: preserve_host=false ignores mixed-case Host headers") {
	static std::shared_ptr<ScopedTestServer> s_upstream;
	static std::shared_ptr<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router upstream;
		upstream.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["host"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		conflux::http::Router front;
		auto popts = conflux::http::ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = s_upstream->port(),
		};
		front.add_context(
			"GET",
			"/echo",
			[popts = std::move(popts)](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await conflux::http::async_proxy(req, popts, ctx.ring);
			});
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	LocalTcpClient client1{s_front->port()};
	auto _ = client1.send("GET /echo HTTP/1.1\r\nHOST: client.example\r\nConnection: close\r\n\r\n");
	auto resp = client1.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == std::format("127.0.0.1:{}", s_upstream->port()));
	LocalTcpClient client2{s_front->port()};
	_ = client2.send("GET /echo HTTP/1.1\r\nHoSt: client.example\r\nConnection: close\r\n\r\n");
	resp = client2.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == std::format("127.0.0.1:{}", s_upstream->port()));
}

TEST_CASE(
	"proxy: path_prefix strips only complete path segments") {
	static std::shared_ptr<ScopedTestServer> s_upstream;
	static std::shared_ptr<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router upstream;
		upstream.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("/"); });
		upstream.get("/users", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("/users");
		});
		upstream.get("/api_v2/users", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("/api_v2/users");
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		conflux::http::Router front;
		auto popts = conflux::http::ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = s_upstream->port(),
			.path_prefix = "/api",
		};
		front.add_context(
			"GET",
			"/api",
			[popts](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await conflux::http::async_proxy(req, popts, ctx.ring);
			});
		front.add_context(
			"GET",
			"/api/users",
			[popts](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await conflux::http::async_proxy(req, popts, ctx.ring);
			});
		front.add_context(
			"GET",
			"/api_v2/users",
			[popts](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await conflux::http::async_proxy(req, popts, ctx.ring);
			});
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	auto resp = http_get_on(s_front->port(), "/api");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "/");
	resp = http_get_on(s_front->port(), "/api/users");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "/users");
	resp = http_get_on(s_front->port(), "/api_v2/users");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "/api_v2/users");
}

TEST_CASE(
	"proxy: appends to existing X-Forwarded-For header") {
	static std::shared_ptr<ScopedTestServer> s_upstream;
	static std::shared_ptr<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router upstream;
		upstream.get("/xff", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["x-forwarded-for"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		conflux::http::Router front;
		auto popts = conflux::http::ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = s_upstream->port(),
		};
		front.add_context(
			"GET",
			"/xff",
			[popts = std::move(popts)](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await conflux::http::async_proxy(req, popts, ctx.ring);
			});
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	auto resp = http_get_on(s_front->port(), "/xff", "X-Forwarded-For: 1.2.3.4\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body = extract_body(resp);
	REQUIRE(body.find("1.2.3.4") != std::string::npos);
	REQUIRE(body.find("127.0.0.1") != std::string::npos);
	REQUIRE(body.find("1.2.3.4, 127.0.0.1") != std::string::npos);
}
