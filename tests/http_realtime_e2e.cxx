#include <catch2/catch_test_macros.hpp>

#include <conflux/detail/discard.hxx>

#include <sys/socket.h>

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
	"WebSocket upgrade performs handshake, echoes text frames, closes cleanly") {
	auto cfg = mw_config();
	cfg.defer_taskrun = true;

	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (auto frame = ws.recv()) {
			if (frame->opcode == conflux::http::WsConn::Opcode::Text) {
				if (!ws.send_text(frame->payload)) {
					break;
				}
			}
		}
	});

	ScopedTestServer srv{cfg, std::move(router)};
	LocalTcpClient client{srv.port()};

	std::string_view const ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
	auto upgrade_req = std::format(
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: {}\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n",
		ws_key);
	auto _ = client.send(upgrade_req);

	std::array<char, 4096> buf{};
	auto n = ::recv(client.fd(), buf.data(), buf.size(), 0);
	REQUIRE(n > 0);
	std::string_view const resp{buf.data(), static_cast<std::size_t>(n)};
	REQUIRE(resp.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
	REQUIRE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string_view::npos);

	std::array<std::uint8_t, 4> const mask = {0x01, 0x02, 0x03, 0x04};
	std::string_view const msg = "hello";
	std::array<std::uint8_t, 11> tx_frame{};
	tx_frame[0] = 0x81;
	tx_frame[1] = 0x80 | 0x05;
	tx_frame[2] = mask[0];
	tx_frame[3] = mask[1];
	tx_frame[4] = mask[2];
	tx_frame[5] = mask[3];
	for (std::size_t i = 0; i < msg.size(); ++i) {
		tx_frame[6 + i] = static_cast<std::uint8_t>(msg[i]) ^ mask[i & 3];
	}
	_ = ::send(client.fd(), tx_frame.data(), tx_frame.size(), 0);

	std::array<std::uint8_t, 64> rx_buf{};
	auto rn = ::recv(client.fd(), rx_buf.data(), rx_buf.size(), 0);
	REQUIRE(rn >= 7);
	REQUIRE(rx_buf[0] == 0x81);
	REQUIRE((rx_buf[1] & 0x80U) == 0U);
	REQUIRE((rx_buf[1] & 0x7FU) == 5U);
	std::string echo{reinterpret_cast<char const *>(rx_buf.data() + 2), 5};
	REQUIRE(echo == "hello");

	std::uint16_t const status = 1000;
	std::array<std::uint8_t, 8> close_frame{};
	close_frame[0] = 0x88;
	close_frame[1] = 0x80 | 0x02;
	close_frame[2] = 0xAA;
	close_frame[3] = 0xBB;
	close_frame[4] = 0xCC;
	close_frame[5] = 0xDD;
	close_frame[6] = static_cast<std::uint8_t>((status >> 8) ^ close_frame[2]);
	close_frame[7] = static_cast<std::uint8_t>((status & 0xFF) ^ close_frame[3]);
	_ = ::send(client.fd(), close_frame.data(), close_frame.size(), 0);

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	srv.stop();
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
