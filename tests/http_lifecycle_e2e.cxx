#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.net.config;
import conflux.net.http_server;
import conflux.net.router;
import conflux.tests.support;

using conflux::http::Config;
using namespace conflux::tests;
namespace chttp = conflux::http;

namespace {

std::string response_body(
	std::string_view response) {
	auto const body_start = response.find("\r\n\r\n");
	REQUIRE(body_start != std::string_view::npos);
	return std::string{response.substr(body_start + 4)};
}

} // namespace

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

	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const port = srv.port();

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

	srv.stop();

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
	"idle connection is closed after request_timeout_ms") {
	Config cfg = mw_config();
	cfg.request_timeout_ms = 1500;

	conflux::http::Router router;
	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });

	ScopedTestServer srv{cfg, std::move(router)};
	auto const timeout_port = srv.port();

	LocalTcpClient client{timeout_port};
	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	auto const close_state = recv_close_state(client.fd(), MSG_DONTWAIT);

	REQUIRE(is_socket_closed(close_state));

	auto resp = http_get_on(timeout_port, "/ok", "Connection: close\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));

	srv.stop();
}

TEST_CASE(
	"slow handler diagnostics: slow sync handler still serves response") {
	Config cfg = mw_config();
	cfg.slow_handler_diagnostics = true;
	cfg.slow_handler_warn_ms = 1;

	conflux::http::Router router;
	router.get("/slow", [](conflux::http::OwnedRequest const &) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		return conflux::http::Response::text("slow-ok");
	});

	ScopedTestServer srv{cfg, std::move(router)};
	auto const port = srv.port();

	auto const started = std::chrono::steady_clock::now();
	auto resp = http_get_on(port, "/slow");
	auto const elapsed_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "slow-ok");
	REQUIRE(elapsed_ms >= 10);

	srv.stop();
}

TEST_CASE(
	"drain closes idle keep-alive connection and reports idle close",
	"[http.lifecycle]") {
	Config cfg = mw_config();
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });

	ScopedTestServer srv{cfg, std::move(router)};
	LocalTcpClient client{srv.port()};
	auto _ = client.send("GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n");
	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Connection: keep-alive") != std::string::npos);

	auto report = srv.drain(chttp::DrainOptions{.deadline = std::chrono::milliseconds{2000}});
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
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });

	ScopedTestServer srv{cfg, std::move(router)};
	auto const port = srv.port();
	auto before = http_get_on(port, "/ping", "Connection: close\r\n");
	REQUIRE(before.starts_with("HTTP/1.1 200 OK"));

	auto report = srv.drain(chttp::DrainOptions{.deadline = std::chrono::milliseconds{2000}});
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
	conflux::http::Router router;
	constexpr auto body_size = 512UZ * 1024UZ;
	router.get("/large", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(std::string(body_size, 'x'));
	});

	ScopedTestServer srv{cfg, std::move(router)};
	LocalTcpClient client{srv.port()};
	auto _ = client.send("GET /large HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	auto headers = client.read_headers();
	REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));

	auto report = srv.drain(chttp::DrainOptions{.deadline = std::chrono::milliseconds{5000}, .finish_requests = true});
	CHECK_FALSE(report.deadline_hit);
	auto resp = headers + client.read_until_close();
	CHECK(resp.starts_with("HTTP/1.1 200 OK"));
	CHECK(resp.find(std::format("Content-Length: {}", body_size)) != std::string::npos);
	auto const header_end = resp.find("\r\n\r\n");
	REQUIRE(header_end != std::string::npos);
	auto const body = std::string_view{resp}.substr(header_end + 4);
	CHECK(body.size() == body_size);
	CHECK(std::ranges::all_of(body, [](char c) { return c == 'x'; }));
}

TEST_CASE(
	"drain deadline reports hit when idle close is disabled",
	"[http.lifecycle]") {
	Config cfg = mw_config();
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });

	ScopedTestServer srv{cfg, std::move(router)};
	LocalTcpClient client{srv.port()};
	auto _ = client.send("GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n");
	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));

	auto report = srv.drain(chttp::DrainOptions{.deadline = std::chrono::milliseconds{1}, .close_idle = false});
	CHECK(report.deadline_hit);
	auto metrics = srv.metrics();
	CHECK(metrics.pressure.drain_started >= 1);
}
