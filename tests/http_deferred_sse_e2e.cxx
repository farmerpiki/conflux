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
using conflux::tests::read_one_response;
using conflux::tests::ScopedTestServer;

// ---------------------------------------------------------------------------
// C1: SseChannel bounded queue + overflow policies (unit tests, no server)
// ---------------------------------------------------------------------------

TEST_CASE(
	"SseChannel: DropNewest policy drops overflowing frames") {
	conflux::http::SseChannel ch{64, conflux::http::SseOverflowPolicy::DropNewest};
	// Each frame is 10 bytes; queue holds at most 64 bytes → 6 fit.
	for (int i = 0; i < 10; ++i) {
		std::string frame(10, 'x');
		[[maybe_unused]] auto const _ = ch.send(std::move(frame));
	}
	REQUIRE(ch.dropped_count() == 4);
	auto out = ch.drain();
	REQUIRE(out.size() == 60);
}
TEST_CASE(
	"SseChannel: DropOldest policy keeps newest frames") {
	conflux::http::SseChannel ch{30, conflux::http::SseOverflowPolicy::DropOldest};
	for (int i = 0; i < 5; ++i) {
		std::string frame(10, static_cast<char>('a' + i));
		[[maybe_unused]] auto const _ = ch.send(std::move(frame));
	}
	REQUIRE(ch.dropped_count() >= 2);
	auto out = ch.drain();
	// After overflow, the final 3 frames (cc…, dd…, ee…) should remain.
	REQUIRE(out.find("eeeeeeeeee") != std::string::npos);
	REQUIRE(out.find("aaaaaaaaaa") == std::string::npos);
}
TEST_CASE(
	"SseChannel: Disconnect policy closes on overflow") {
	conflux::http::SseChannel ch{20, conflux::http::SseOverflowPolicy::Disconnect};
	REQUIRE(ch.send(std::string(10, 'x')));
	// Next send exceeds the cap → channel is closed; further sends return false.
	REQUIRE_FALSE(ch.send(std::string(20, 'y')));
	REQUIRE(ch.is_closed());
	REQUIRE_FALSE(ch.send(std::string(5, 'z')));
}
TEST_CASE(
	"SseChannel: send returns false after close") {
	conflux::http::SseChannel ch{4096};
	ch.close();
	REQUIRE_FALSE(ch.send("hello"));
}
// ---------------------------------------------------------------------------
// C2: conflux::http::DeferredResponse timeout
// ---------------------------------------------------------------------------

TEST_CASE(
	"deferred response that never completes returns 504 after its deadline") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 30000;

	conflux::http::Router router;
	// Handler returns a conflux::http::DeferredResponse with a 1-second deadline but never completes it.
	// The idle-timer sweeper should expire it with a 504 shortly after.
	router.get("/stuck", [](conflux::http::OwnedRequest const &) {
		auto d = std::make_shared<conflux::http::DeferredResponse>(std::chrono::milliseconds{1000});
		return conflux::http::Response::deferred(d);
	});

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const p = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(p);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	std::string_view const req = "GET /stuck HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);

	auto const response = read_one_response(fd);
	::close(fd);
	REQUIRE(response.starts_with("HTTP/1.1 504"));
	srv.stop();
}
TEST_CASE(
	"deferred response that completes before its deadline returns the completed payload") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	conflux::http::Router router;
	router.get("/fast", [](conflux::http::OwnedRequest const &) {
		auto d = std::make_shared<conflux::http::DeferredResponse>(std::chrono::milliseconds{10000});
		std::thread([d]() {
			std::this_thread::sleep_for(std::chrono::milliseconds{80});
			d->complete(conflux::http::Response::text("pong"));
		}).detach();
		return conflux::http::Response::deferred(d);
	});

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const p = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(p);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	std::string_view const req = "GET /fast HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);

	auto const response = read_one_response(fd);
	::close(fd);
	REQUIRE(response.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response.find("pong") != std::string::npos);
	srv.stop();
}
