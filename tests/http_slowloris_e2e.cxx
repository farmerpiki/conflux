// Plain TU: raw socket e2e coverage for timeout paths.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string_view>
#include <sys/socket.h>
#include <thread>

import std;
import conflux.net.http;
import conflux.tests.support;

using namespace conflux::tests;
namespace chttp = conflux::http;

TEST_CASE(
	"HTTP/1 slowloris incomplete headers return 408 and metric",
	"[http][e2e][slowloris]") {
	Config cfg = Config::test();
	cfg.request_timeout_ms = 1000;
	cfg.tls_sniff_timeout_ms = 0;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	cfg.send_buffer_slabs = 0;

	Router router;
	router.get("/", [](chttp::Request const &) { return chttp::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};

	LocalTcpClient client{srv.port()};
	client.set_recv_timeout(std::chrono::seconds{5});

	std::string_view const partial = "GET / HTTP/1.1\r\nHost: localhost\r\n";
	for (char ch: partial) {
		auto const sent = client.send(std::string_view{&ch, 1}, MSG_NOSIGNAL);
		if (sent <= 0) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{25});
	}

	auto response = client.read_one_response();
	REQUIRE(response.starts_with("HTTP/1.1 408 Request Timeout"));
	CHECK(response.find("application/problem+json") != std::string::npos);
	CHECK(response.find(R"("code":"header_timeout")") != std::string::npos);
	CHECK(response.find(R"("diagnostic_code":"header_timeout")") != std::string::npos);

	auto const metrics = srv.metrics();
	CHECK(metrics.rejections.header_timeout >= 1);
}

TEST_CASE(
	"HTTP/1 partial request body returns 408 and metric",
	"[http][e2e][timeout]") {
	Config cfg = Config::test();
	cfg.request_timeout_ms = 1000;
	cfg.tls_sniff_timeout_ms = 0;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	cfg.send_buffer_slabs = 0;

	Router router;
	router.post("/upload", [](chttp::Request const &req) { return chttp::Response::text(req.body); });
	ScopedTestServer srv{cfg, std::move(router)};

	LocalTcpClient client{srv.port()};
	client.set_recv_timeout(std::chrono::seconds{5});

	std::string_view const partial =
		"POST /upload HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Length: 5\r\n"
		"Connection: close\r\n"
		"\r\n"
		"he";
	REQUIRE(client.send(partial, MSG_NOSIGNAL) == static_cast<ssize_t>(partial.size()));

	auto response = client.read_one_response();
	REQUIRE(response.starts_with("HTTP/1.1 408 Request Timeout"));
	CHECK(response.find("application/problem+json") != std::string::npos);
	CHECK(response.find(R"("code":"body_timeout")") != std::string::npos);
	CHECK(response.find(R"("diagnostic_code":"body_timeout")") != std::string::npos);

	auto const metrics = srv.metrics();
	CHECK(metrics.rejections.body_timeout >= 1);
}
