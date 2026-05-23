// Plain TU: raw socket e2e coverage for bounded lifecycle/backpressure paths.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>

import std;
import conflux.net.http;
import conflux.tests.support;

using namespace conflux::tests;
namespace chttp = conflux::http;
namespace {

Config backpressure_cfg() {
	Config cfg = Config::test();
	cfg.rings = 1;
	cfg.ring_entries = 64;
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

} // namespace

TEST_CASE(
	"drain force-closes live connections after deadline and records pressure metrics",
	"[http][e2e][backpressure]") {
	Router router;
	router.get("/large", [](chttp::Request const &) { return chttp::Response::text(std::string(1024, 'x')); });
	ScopedTestServer srv{backpressure_cfg(), std::move(router)};

	LocalTcpClient client{srv.port()};
	std::string_view const req = "GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n";
	REQUIRE(client.send(req, MSG_NOSIGNAL) == static_cast<ssize_t>(req.size()));
	auto const headers = client.read_headers();
	REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));

	auto report = srv.drain(
		DrainOptions{
			.deadline = std::chrono::milliseconds{1},
			.close_idle = false,
			.finish_requests = true,
		});
	CHECK(report.deadline_hit);

	auto const metrics = srv.metrics();
	CHECK(metrics.pressure.drain_started >= 1);
	CHECK(metrics.pressure.drain_deadline_hit >= 1);
	CHECK(metrics.pressure.drain_forced_close >= 1);
	client.close();
}

TEST_CASE(
	"drain closes an open SSE stream and records stream pressure metrics",
	"[http][e2e][backpressure][sse][lifecycle]") {
	Router router;
	router.get("/events/open", [](chttp::Request const &) {
		auto ch = std::make_shared<SseChannel>();
		(void)ch->send("data: open\n\n");
		return chttp::Response::sse(std::move(ch));
	});
	ScopedTestServer srv{backpressure_cfg(), std::move(router)};

	LocalTcpClient client{srv.port()};
	std::string_view const req = "GET /events/open HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n";
	REQUIRE(client.send(req, MSG_NOSIGNAL) == static_cast<ssize_t>(req.size()));

	auto headers = client.read_headers();
	REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(headers.find("Content-Type: text/event-stream") != std::string::npos);

	auto report = srv.drain(
		DrainOptions{
			.deadline = std::chrono::milliseconds{2000},
			.finish_requests = true,
			.finish_streams = false,
			.sse_policy = DrainStreamPolicy::close,
		});
	CHECK_FALSE(report.deadline_hit);
	CHECK(report.streams_closed >= 1);

	auto const metrics = srv.metrics();
	CHECK(metrics.pressure.drain_started >= 1);
	CHECK(metrics.pressure.connections_closed_for_pressure >= 1);
	client.close();
}
