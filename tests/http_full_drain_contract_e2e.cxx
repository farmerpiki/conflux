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

constexpr std::size_t kLargeBodyBytes = 2UZ * 1024UZ * 1024UZ;

chttp::Config drain_contract_cfg() {
	chttp::Config cfg = chttp::Config::test();
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

[[nodiscard]] bool connect_refused(
	std::uint16_t port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	auto const rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	auto const connect_errno = errno;
	::close(fd);
	if (rc < 0) {
		return connect_errno == ECONNREFUSED || connect_errno == ECONNRESET || connect_errno == ENOENT;
	}
	return false;
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
		return chttp::Response::text(std::string(kLargeBodyBytes, 'x'));
	});
	router.get("/events/open", [](chttp::RequestView const &) {
		auto ch = std::make_shared<conflux::http::SseChannel>();
		(void)ch->send("data: open\n\n");
		return chttp::Response::sse(std::move(ch));
	});
	router.ws("/ws", [](chttp::RequestView const &, conflux::http::WsConn &ws) {
		while (auto frame = ws.recv()) {
			if (frame->opcode == conflux::http::WsConn::Opcode::Text && !ws.send_text(frame->payload)) {
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
	set_recv_timeout(large.fd(), std::chrono::seconds{30});
	std::string_view const large_req = "GET /large HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	REQUIRE(large.send(large_req, MSG_NOSIGNAL) == static_cast<ssize_t>(large_req.size()));
	auto large_headers = large.read_headers();
	REQUIRE(large_headers.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(large_headers.find(std::format("Content-Length: {}", kLargeBodyBytes)) != std::string::npos);

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

	std::optional<chttp::DrainReport> report;
	std::thread drain_thread{[&] {
		report = srv.drain(
			chttp::DrainOptions{
				.deadline = std::chrono::milliseconds{30000},
				.stop_accepting = true,
				.close_idle = true,
				.finish_requests = true,
				.finish_streams = false,
				.websocket_policy = chttp::DrainStreamPolicy::close,
				.sse_policy = chttp::DrainStreamPolicy::close,
			});
	}};

	std::this_thread::sleep_for(std::chrono::milliseconds{50});
	auto large_tail = read_until_close_with_state(large.fd());
	drain_thread.join();
	REQUIRE(report.has_value());
	REQUIRE(large_tail.closed());

	CHECK_FALSE(report->deadline_hit);
	CHECK(report->accepted_before_stop >= 3);
	CHECK(report->idle_closed >= 1);
	CHECK(report->requests_finished >= 1);
	CHECK(report->streams_closed >= 2);
	CHECK(report->forced_closed == 0);
	auto const large_header_end = large_headers.find("\r\n\r\n");
	REQUIRE(large_header_end != std::string::npos);
	std::string large_body{large_headers.substr(large_header_end + 4)};
	large_body += large_tail.bytes;
	CHECK(large_body.size() == kLargeBodyBytes);
	CHECK(std::ranges::all_of(large_body, [](char c) { return c == 'x'; }));

	char probe{};
	set_recv_timeout(idle.fd(), std::chrono::milliseconds{250});
	CHECK(idle.recv(&probe, 1) == 0);
	set_recv_timeout(sse.fd(), std::chrono::milliseconds{250});
	auto const sse_tail = read_until_close_with_state(sse.fd());
	CHECK(sse_tail.closed());
	CHECK(sse_tail.bytes.find("0\r\n\r\n") != std::string::npos);
	set_recv_timeout(ws.fd(), std::chrono::milliseconds{250});
	auto const ws_close = recv_close_state(ws.fd());
	CHECK(is_socket_closed(ws_close));
	CHECK(connect_refused(port));

	auto const metrics = srv.metrics();
	CHECK(metrics.pressure.drain_started >= 1);
	CHECK(metrics.pressure.connections_closed_for_pressure >= 1);
	CHECK(metrics.pressure.websocket_closed_for_pressure >= 1);
}
