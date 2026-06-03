#include <catch2/catch_test_macros.hpp>

#include <conflux/detail/discard.hxx>

import std;
import conflux.net.app.defer;
import conflux.net.config;
import conflux.net.http.realtime;
import conflux.net.router;
import conflux.tests.support;
import conflux.work;

using namespace conflux::tests;
using conflux::work::WorkPool;
using conflux::work::WorkPoolOptions;

namespace {

std::uint16_t g_realtime_port = 0;

void ensure_realtime_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		cfg.defer_taskrun = true;

		conflux::http::Router router;
		auto defer_ok_pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 1, .max_inject_queue = 16});
		auto defer_full_pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 1});
		defer_full_pool->stop();
		router.get("/api/defer-ok", [defer_ok_pool](conflux::http::OwnedRequest const &) {
			return conflux::http::defer(defer_ok_pool, [] {
				return conflux::http::Response::json(R"({"defer":"ok"})");
			});
		});
		router.get("/api/defer-full", [defer_full_pool](conflux::http::OwnedRequest const &) {
			return conflux::http::defer(defer_full_pool, [] {
				return conflux::http::Response::json(R"({"defer":"unreachable"})");
			});
		});
		router.sse(
			"/events",
			[](conflux::http::OwnedRequest const &, std::shared_ptr<conflux::http::SseChannel> const &ch) {
				auto _ = ch->send("data: event1\n\n");
				CONFLUX_DISCARD(ch->send("data: event2\n\n"));
				CONFLUX_DISCARD(ch->send("data: event3\n\n"));
				ch->close();
			});
		router.sse(
			"/events/{name}",
			[](conflux::http::OwnedRequest const &req, std::shared_ptr<conflux::http::SseChannel> const &ch) {
				auto _ = ch->send(std::format("data: hello {}\n\n", req.params["name"]));
				ch->close();
			});

		g_realtime_port = test_servers().start(cfg, std::move(router));
	});
}

std::string realtime_get(
	std::string_view path) {
	ensure_realtime_server();
	return conflux::tests::http_get_on(g_realtime_port, path);
}

std::string realtime_sse_get(
	std::string_view path) {
	ensure_realtime_server();

	LocalTcpClient client{g_realtime_port};
	auto req_str = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n", path);
	auto _ = client.send(req_str);
	client.set_recv_timeout(std::chrono::seconds{5});
	return client.read_until_close();
}

std::string_view response_body(
	std::string_view response) {
	auto body_start = response.find("\r\n\r\n");
	REQUIRE(body_start != std::string_view::npos);
	return response.substr(body_start + 4);
}

} // namespace

TEST_CASE(
	"GET /api/defer-ok returns deferred payload") {
	auto resp = realtime_get("/api/defer-ok");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find(R"("defer":"ok")") != std::string::npos);
}

TEST_CASE(
	"GET /api/defer-full returns queue-full error") {
	auto resp = realtime_get("/api/defer-full");
	REQUIRE(resp.starts_with("HTTP/1.1 500 Internal Server Error"));
	REQUIRE(resp.find("offload queue full") != std::string::npos);
}

TEST_CASE(
	"SSE /events returns text/event-stream") {
	auto resp = realtime_sse_get("/events");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Type: text/event-stream") != std::string::npos);
}

TEST_CASE(
	"SSE /events streams all 3 events") {
	auto resp = realtime_sse_get("/events");
	auto body = response_body(resp);
	REQUIRE(body.find("data: event1") != std::string_view::npos);
	REQUIRE(body.find("data: event2") != std::string_view::npos);
	REQUIRE(body.find("data: event3") != std::string_view::npos);
}

TEST_CASE(
	"SSE /events/{name} captures param") {
	auto resp = realtime_sse_get("/events/alice");
	auto body = response_body(resp);
	REQUIRE(body.find("data: hello alice") != std::string_view::npos);
}

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
	}
	bc.broadcast_data("ping");
	REQUIRE(bc.subscriber_count() == 0);
}
