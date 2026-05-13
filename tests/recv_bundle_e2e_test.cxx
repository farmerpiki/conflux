// Plain TU — not a module unit.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import conflux.tests.support;

using namespace conflux::tests;
namespace {

// Smallest legal ring; buf_ring count = 8*4 = 32 slots.
// 80 rapid connect-send-close cycles exhaust the ring twice over if
// buffers are not recycled on RST/abrupt close.
Config small_ring_cfg() {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 8;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.startup_banner = false;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	cfg.recv_bundle = true;
	cfg.direct_accept = true;
	return cfg;
}
Config small_ring_cfg_non_direct_accept() {
	auto cfg = small_ring_cfg();
	cfg.direct_accept = false;
	return cfg;
}
// Open a raw TCP socket, send a partial HTTP request, then close immediately.
// Does NOT wait for a response — goal is to force the server to handle an
// abrupt RST while a recv buffer may still be pinned.
void send_and_abort(
	u16 port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		return;
	}
	// Partial request — no terminal \r\n\r\n so the server cannot dispatch it.
	char const partial[] = "GET /api/ping HTTP/1.1\r\nHost: 127.0.0.1\r\n";
	auto _ = ::write(fd, partial, sizeof(partial) - 1);
	::close(fd);
}

} // namespace
// Test 14: buf_ring survives rapid connect-send-close flood
// 80 abrupt connections each consume a recv buffer; the ring has only 32 slots.
// Correct recycling (via discard_recv_bufs on error paths) keeps the ring live.
// A final round-trip proves the server is still accepting requests.
TEST_CASE(
	"recv_bundle.e2e: buf_ring survives abrupt-close flood",
	"[recv_bundle][e2e]") {
	Router router;
	router.get("/api/ping", [](HttpRequest const &) { return HttpResponse::json(R"({"status":"ok"})"); });
	ScopedTestServer srv{small_ring_cfg(), move(router)};
	u16 const port = srv.port();

	// Flood: 80 connections each send a partial request and close abruptly.
	// The kernel RSTs the connection; the server must recycle the recv buffer.
	for (int i = 0; i < 80; ++i) {
		send_and_abort(port);
	}

	// Drain — give the server time to process the RSTs and recycle all buffers.
	std::this_thread::sleep_for(chrono::milliseconds(200));

	// Ring must still be operational: 4 full round-trips succeed.
	for (int i = 0; i < 4; ++i) {
		S const resp = http_get_on(port, "/api/ping");
		CHECK(resp.find("200 OK") != S::npos);
		CHECK(resp.find("\"status\":\"ok\"") != S::npos);
	}
}
TEST_CASE(
	"recv_bundle.e2e: buf_ring survives abrupt-close flood without direct accept",
	"[recv_bundle][e2e]") {
	Router router;
	router.get("/api/ping", [](HttpRequest const &) { return HttpResponse::json(R"({"status":"ok"})"); });
	ScopedTestServer srv{small_ring_cfg_non_direct_accept(), move(router)};
	u16 const port = srv.port();

	for (int i = 0; i < 80; ++i) {
		send_and_abort(port);
	}

	std::this_thread::sleep_for(chrono::milliseconds(200));

	for (int i = 0; i < 4; ++i) {
		S const resp = http_get_on(port, "/api/ping");
		CHECK(resp.find("200 OK") != S::npos);
		CHECK(resp.find("\"status\":\"ok\"") != S::npos);
	}
}
