#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.http.extended;
import conflux.net.config;
import conflux.net.router;
import conflux.tests.support;

using conflux::http::Config;
using conflux::tests::is_socket_closed;
using conflux::tests::recv_close_state;
using conflux::tests::ScopedTestServer;

TEST_CASE(
	"TLS sniff: silent connection closed after tls_sniff_timeout_ms") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 0;
	cfg.tls_sniff_timeout_ms = 1500;
	conflux::tests::configure_test_tls(cfg);

	conflux::http::Router router;
	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const port = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	auto const end = recv_close_state(fd, MSG_DONTWAIT);
	::close(fd);
	REQUIRE(is_socket_closed(end));

	srv.stop();
}

TEST_CASE(
	"TLS sniff: client half-close before any data triggers clean server close") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 30000;
	cfg.tls_sniff_timeout_ms = 30000;
	conflux::tests::configure_test_tls(cfg);

	conflux::http::Router router;
	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const port = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	::shutdown(fd, SHUT_WR);

	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	auto const end = recv_close_state(fd, MSG_DONTWAIT);
	::close(fd);
	REQUIRE(is_socket_closed(end));

	srv.stop();
}
