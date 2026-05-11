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
import conflux.net.http;
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

} // namespace
TEST_CASE(
	"http.cq_overflow: non-UB shutdown under small-CQ flood",
	"[.flaky]") {
	Router router;
	router.get("/ping", [](HttpRequest const &) { return HttpResponse::text("pong"); });

	auto cfg = tiny_ring_config();
	auto server = make_shared<HttpServer>(cfg, move(router));
	RunStatus result = RunStatus::stopped_normally;
	Atom<bool> srv_exited{false};
	jthread srv_thread([&] {
		result = server->run();
		srv_exited.store(true, memory_order_release);
	});

	u16 const port = server->port();
	wait_for_server(port);

	// Fire concurrent requests; keep going until the server stops or 3 s elapse.
	Atom<bool> stop_flag{false};
	static constexpr int kWorkers = 8;
	static constexpr int kIterations = 2000;
	V<jthread> workers;
	workers.reserve(kWorkers);
	for (int w = 0; w < kWorkers; ++w) {
		workers.emplace_back([port, &stop_flag] {
			for (int i = 0; i < kIterations && !stop_flag.load(memory_order_relaxed); ++i) {
				try {
					(void)http_get_on(port, "/ping");
				} catch (...) { break; }
			}
		});
	}

	// Wait up to 3 s for the server thread to exit (fatal or normal).
	static constexpr int kPollMs = 10;
	static constexpr int kTimeoutMs = 3000;
	bool server_stopped = false;
	for (int elapsed = 0; elapsed < kTimeoutMs; elapsed += kPollMs) {
		if (srv_exited.load(memory_order_acquire)) {
			server_stopped = true;
			break;
		}
		std::this_thread::sleep_for(chrono::milliseconds(kPollMs));
	}

	stop_flag.store(true, memory_order_relaxed);

	// Give server a clean shutdown if it's still running.
	if (!server_stopped) {
		server->shutdown();
	}

	srv_thread.join();
	workers.clear(); // join all workers

	// Any valid RunStatus is acceptable — the point is no crash or UB.
	bool const valid_status = result == RunStatus::stopped_normally
						   || result == RunStatus::fatal_cq_overflow
						   || result == RunStatus::fatal_cq_overflow_no_nodrop
						   || result == RunStatus::fatal_submit_wait_ebadr
						   || result == RunStatus::fatal_internal_exception;
	CHECK(valid_status);
}
