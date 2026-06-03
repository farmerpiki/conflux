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
	char cert_tmp[] = "/tmp/conflux_sniff_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_sniff_key_XXXXXX.pem";
	{
		int fd = ::mkstemps(cert_tmp, 4);
		::close(fd);
		fd = ::mkstemps(key_tmp, 4);
		::close(fd);
	}
	std::string const cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	REQUIRE(::system(cmd.c_str()) == 0);

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
	cfg.cert_file = cert_tmp;
	cfg.key_file = key_tmp;

	conflux::http::Router router;
	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const port = srv.port();
	::unlink(cert_tmp);
	::unlink(key_tmp);

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
	char cert_tmp[] = "/tmp/conflux_eof_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_eof_key_XXXXXX.pem";
	{
		int fd = ::mkstemps(cert_tmp, 4);
		::close(fd);
		fd = ::mkstemps(key_tmp, 4);
		::close(fd);
	}
	std::string const cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	REQUIRE(::system(cmd.c_str()) == 0);

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
	cfg.cert_file = cert_tmp;
	cfg.key_file = key_tmp;

	conflux::http::Router router;
	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const port = srv.port();
	::unlink(cert_tmp);
	::unlink(key_tmp);

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
