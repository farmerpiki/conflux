#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.router;
import conflux.net.vhost;
import conflux.tests.support;
import conflux.work;

using namespace conflux::tests;

namespace {

using conflux::work::WorkPool;

std::uint16_t g_vhost_port = 0;
std::uint16_t g_vhost_direct_port = 0;

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

void ensure_vhost_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router api_router;
		api_router.get("/status", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("api");
		});

		conflux::http::Router web_router;
		web_router.get("/status", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("web");
		});

		conflux::http::Router def_router;
		def_router.get("/status", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("default");
		});

		auto vhr = std::make_shared<conflux::http::VHostRouter>();
		vhr->add("api.example.com", std::move(api_router));
		vhr->add("web.example.com", std::move(web_router));
		vhr->set_default(std::move(def_router));

		conflux::http::Router main;
		main.use([vhr](conflux::http::OwnedRequest const &req, conflux::http::Router::Handler const &) {
			return vhr->dispatch(req);
		});
		g_vhost_port = start_mw_server(mw_config(), std::move(main));
	});
}

void ensure_vhost_direct_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router api_router;
		api_router.get("/status", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("api-direct");
		});

		conflux::http::Router def_router;
		def_router.get("/status", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("default-direct");
		});

		conflux::http::VHostRouter vhost_router;
		vhost_router.add("api.example.com", std::move(api_router));
		vhost_router.set_default(std::move(def_router));

		g_vhost_direct_port = test_servers().start(mw_config(), std::move(vhost_router));
	});
}

} // namespace

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

	conflux::http::Router api_router;
	conflux::http::Router web_router;
	conflux::http::Router def_router;

	conflux::http::VHostRouter vhost;
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
	"router: work pool is created only for websocket or sse routes") {
	conflux::http::Router plain;
	CHECK_FALSE(plain.work_pool());

	conflux::http::Router sse;
	sse.sse("/events", [](conflux::http::RequestView const &, std::shared_ptr<conflux::http::SseChannel> const &) {});
	CHECK(sse.work_pool());

	conflux::http::Router ws;
	ws.ws("/ws", [](conflux::http::RequestView const &, conflux::http::WsConn &) {});
	CHECK(ws.work_pool());
}

TEST_CASE(
	"vhost: default construction does not allocate work pool") {
	conflux::http::VHostRouter vhost;
	CHECK_FALSE(vhost.work_pool());

	conflux::http::Router plain;
	vhost.add("plain.example.com", std::move(plain));
	CHECK_FALSE(vhost.resolved_work_pool("plain.example.com"));

	conflux::http::Router sse;
	sse.sse("/events", [](conflux::http::RequestView const &, std::shared_ptr<conflux::http::SseChannel> const &) {});
	auto sse_pool = sse.work_pool();
	REQUIRE(sse_pool);
	vhost.add("sse.example.com", std::move(sse));
	CHECK(vhost.resolved_work_pool("sse.example.com").get() == sse_pool.get());
}

TEST_CASE(
	"vhost: rebinding work pool updates existing subrouters") {
	conflux::http::Router api_router;
	conflux::http::Router def_router;

	conflux::http::VHostRouter vhost;
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
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router api;
		api.get("/status", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("api"); });
		conflux::http::VHostRouter vhost;
		vhost.add("api.example.com", std::move(api));
		conflux::http::Config const cfg{.port = 0, .rings = 1};
		port = test_servers().start(cfg, std::move(vhost));
	});
	auto resp = http_get_on_host(port, "unknown.example.com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 404"));
}

TEST_CASE(
	"vhost: IPv6 host with port is stripped before matching") {
	conflux::http::Router api;
	api.get("/status", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("api-v6"); });
	conflux::http::VHostRouter vhost;
	vhost.add("[::1]", std::move(api));

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/status";
	req.headers["host"] = "[::1]:8080";

	auto resp = vhost.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "api-v6");
}

TEST_CASE(
	"vhost: IPv6 host without port matches directly") {
	conflux::http::Router api;
	api.get("/status", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text("api-v6-noport");
	});
	conflux::http::VHostRouter vhost;
	vhost.add("[::1]", std::move(api));

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/status";
	req.headers["host"] = "[::1]";

	auto resp = vhost.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "api-v6-noport");
}
