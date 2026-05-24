// Plain TU: contract-level HTTP drain lifecycle coverage.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

import std;
import conflux.net.config;
import conflux.net.http.realtime;
import conflux.net.http_server;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;
namespace chttp = conflux::http;
namespace {

Config drain_contract_cfg() {
	Config cfg = Config::test();
	cfg.rings = 1;
	cfg.ring_entries = 128;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 0;
	cfg.tls_sniff_timeout_ms = 0;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	cfg.send_buffer_slabs = 0;
	return cfg;
}

void set_recv_timeout(
	int fd,
	std::chrono::milliseconds timeout) {
	timeval tv{
		.tv_sec = static_cast<time_t>(timeout.count() / 1000),
		.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000),
	};
	(void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void shrink_recv_buffer(
	int fd) {
	int size = 4096;
	(void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

std::string read_until_close_from(
	int fd) {
	std::string out;
	std::array<char, 8192> buf{};
	for (;;) {
		auto const n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n <= 0) {
			break;
		}
		out.append(buf.data(), static_cast<std::size_t>(n));
	}
	return out;
}

[[nodiscard]] bool connect_and_expect_no_service(
	std::uint16_t port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	auto const rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	if (rc < 0) {
		::close(fd);
		return true;
	}
	set_recv_timeout(fd, std::chrono::milliseconds{200});
	std::string_view const probe = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	(void)::send(fd, probe.data(), probe.size(), MSG_NOSIGNAL);
	char byte{};
	auto const n = ::recv(fd, &byte, 1, 0);
	::close(fd);
	return n <= 0;
}

std::string websocket_upgrade_request() {
	return std::string{
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n"};
}

} // namespace

TEST_CASE(
	"drain contract stops accepts and drains idle, active, SSE, and WebSocket connections",
	"[http][e2e][lifecycle][drain]") {
	Router router;
	router.get("/ping", [](chttp::RequestView const &) { return chttp::Response::text("pong"); });
	router.get("/large", [](chttp::RequestView const &) {
		return chttp::Response::text(std::string(16 * 1024 * 1024, 'x'));
	});
	router.get("/events/open", [](chttp::RequestView const &) {
		auto ch = std::make_shared<SseChannel>();
		(void)ch->send("data: open\n\n");
		return chttp::Response::sse(std::move(ch));
	});
	router.ws("/ws", [](chttp::RequestView const &, WsConn &ws) {
		while (auto frame = ws.recv()) {
			if (frame->opcode == WsConn::Opcode::Text && !ws.send_text(frame->payload)) {
				break;
			}
		}
	});

	ScopedTestServer srv{drain_contract_cfg(), std::move(router)};
	auto const port = srv.port();

	LocalTcpClient idle{port};
	std::string_view const ping_req = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
	REQUIRE(idle.send(ping_req, MSG_NOSIGNAL) == static_cast<ssize_t>(ping_req.size()));
	auto idle_resp = idle.read_one_response();
	REQUIRE(idle_resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(idle_resp.find("Connection: keep-alive") != std::string::npos);

	LocalTcpClient large{port};
	shrink_recv_buffer(large.fd());
	set_recv_timeout(large.fd(), std::chrono::seconds{5});
	std::string_view const large_req = "GET /large HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	REQUIRE(large.send(large_req, MSG_NOSIGNAL) == static_cast<ssize_t>(large_req.size()));
	auto large_headers = large.read_headers();
	REQUIRE(large_headers.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(large_headers.find("Content-Length: 16777216") != std::string::npos);

	LocalTcpClient sse{port};
	std::string_view const sse_req =
		"GET /events/open HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n";
	REQUIRE(sse.send(sse_req, MSG_NOSIGNAL) == static_cast<ssize_t>(sse_req.size()));
	auto sse_headers = sse.read_headers();
	REQUIRE(sse_headers.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(sse_headers.find("Content-Type: text/event-stream") != std::string::npos);
	set_recv_timeout(sse.fd(), std::chrono::milliseconds{250});
	std::string sse_initial;
	std::array<char, 256> sse_buf{};
	for (;;) {
		auto const n = sse.recv(sse_buf.data(), sse_buf.size());
		if (n <= 0) {
			break;
		}
		sse_initial.append(sse_buf.data(), static_cast<std::size_t>(n));
	}
	REQUIRE(sse_initial.find("data: open") != std::string::npos);

	LocalTcpClient ws{port};
	auto ws_req = websocket_upgrade_request();
	REQUIRE(ws.send(ws_req, MSG_NOSIGNAL) == static_cast<ssize_t>(ws_req.size()));
	auto ws_headers = ws.read_headers();
	REQUIRE(ws_headers.starts_with("HTTP/1.1 101 Switching Protocols"));

	std::optional<DrainReport> report;
	std::thread drain_thread{[&] {
		report = srv.drain(
			DrainOptions{
				.deadline = std::chrono::milliseconds{5000},
				.stop_accepting = true,
				.close_idle = true,
				.finish_requests = true,
				.finish_streams = false,
				.websocket_policy = DrainStreamPolicy::close,
				.sse_policy = DrainStreamPolicy::close,
			});
	}};

	std::this_thread::sleep_for(std::chrono::milliseconds{50});
	auto large_tail = read_until_close_from(large.fd());
	drain_thread.join();
	REQUIRE(report.has_value());

	CHECK_FALSE(report->deadline_hit);
	CHECK(report->accepted_before_stop >= 4);
	CHECK(report->idle_closed >= 1);
	CHECK(report->requests_finished >= 1);
	CHECK(report->streams_closed >= 2);
	CHECK(report->forced_closed == 0);
	CHECK(!large_tail.empty());
	CHECK(large_tail.ends_with(std::string(32, 'x')));

	char probe{};
	set_recv_timeout(idle.fd(), std::chrono::milliseconds{250});
	CHECK(idle.recv(&probe, 1) == 0);
	set_recv_timeout(sse.fd(), std::chrono::milliseconds{250});
	auto const sse_tail = read_until_close_from(sse.fd());
	CHECK(sse_tail.find("0\r\n\r\n") != std::string::npos);
	set_recv_timeout(ws.fd(), std::chrono::milliseconds{250});
	CHECK(ws.recv(&probe, 1) <= 0);
	CHECK(connect_and_expect_no_service(port));

	auto const metrics = srv.metrics();
	CHECK(metrics.pressure.drain_started >= 1);
	CHECK(metrics.pressure.connections_closed_for_pressure >= 1);
	CHECK(metrics.pressure.websocket_closed_for_pressure >= 1);
}
