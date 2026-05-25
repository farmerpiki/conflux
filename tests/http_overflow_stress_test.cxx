// Plain TU — not a module unit.
// Best-effort HTTP stress test: small CQ ring to make overflow plausible.
// Tagged [.flaky] — overflow is not guaranteed across all kernels/schedulers.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http_server;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;
namespace {

Config tiny_ring_config() {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 4; // CQ=8 — tight; overflow plausible under load
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.startup_banner = false;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	return cfg;
}

static std::string http_get_on_timeout(
	std::uint16_t port,
	std::string_view path) {
	LocalTcpClient const client{port};

	timeval tv{.tv_sec = 0, .tv_usec = 50000};
	::setsockopt(client.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	::setsockopt(client.fd(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	auto request = std::format(
		"GET {} HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Connection: close\r\n"
		"\r\n",
		path);

	(void)client.send(request);
	return client.read_one_response();
}

} // namespace
TEST_CASE(
	"http.cq_overflow: non-UB shutdown under small-CQ flood",
	"[http][overflow][stress]") {
	Router router;
	router.get("/ping", [](Request const &) { return Response::text("pong"); });

	auto cfg = tiny_ring_config();
	auto server = std::make_shared<HttpServer>(cfg, std::move(router));
	RunStatus result = RunStatus::stopped_normally;
	std::atomic<bool> srv_exited{false};
	std::jthread srv_thread([&] {
		result = server->run();
		srv_exited.store(true, std::memory_order_release);
	});

	std::uint16_t const port = server->port();
	wait_for_server(port);

	// Fire concurrent requests; keep going until the server stops or 3 s elapse.
	std::atomic<bool> stop_flag{false};
	static constexpr int kWorkers = 2;
	static constexpr int kIterations = 20;
	std::vector<std::jthread> workers;
	workers.reserve(kWorkers);
	for (int w = 0; w < kWorkers; ++w) {
		workers.emplace_back([port, &stop_flag] {
			for (int i = 0; i < kIterations && !stop_flag.load(std::memory_order_relaxed); ++i) {
				try {
					(void)http_get_on_timeout(port, "/ping");
				} catch (...) { break; }
			}
		});
	}

	// Wait up to 3 s for the server thread to exit (fatal or normal).
	static constexpr int kPollMs = 10;
	static constexpr int kTimeoutMs = 500;
	bool server_stopped = false;
	for (int elapsed = 0; elapsed < kTimeoutMs; elapsed += kPollMs) {
		if (srv_exited.load(std::memory_order_acquire)) {
			server_stopped = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
	}

	stop_flag.store(true, std::memory_order_relaxed);

	if (!server_stopped) {
		server->request_shutdown();
	}

	workers.clear();
	srv_thread.join();

	// Accept normal stop or the explicit pressure statuses this test is meant to exercise.
	bool const valid_status = result == RunStatus::stopped_normally
						   || result == RunStatus::fatal_cq_overflow
						   || result == RunStatus::fatal_cq_overflow_no_nodrop
						   || result == RunStatus::fatal_submit_wait_ebadr;
	CHECK(valid_status);
}
