// Build and run: build/release-clang-libcxx/conflux_http_lifecycle_example
#include <csignal>
#include <cstdio>

import conflux.extended;
import conflux.net.http.realtime;
import std;

namespace http = conflux::http;
using namespace std::chrono_literals;

namespace {

volatile sig_atomic_t stop_requested = 0;

void handle_stop_signal(
	int) {
	stop_requested = 1;
}

} // namespace

int main() {
	auto app = http::app(http::Config::public_server());

	auto events = std::make_shared<http::SseChannel>(
		http::SseChannel::kDefaultMaxQueueBytes,
		conflux::http::SseOverflowPolicy::DropNewest);

	app.get("/health", [] { return http::text("ok"); });
	app.get("/events", [events] { return http::sse(events); });

	auto server = std::move(app).prepare_server({.port = 8080});
	if (!server) {
		std::println(std::cerr, "server setup failed: {}", server.error());
		return 1;
	}

	::signal(SIGINT, handle_stop_signal);
	::signal(SIGTERM, handle_stop_signal);

	std::jthread signal_thread{[&] {
		while (stop_requested == 0) {
			std::this_thread::sleep_for(100ms);
		}
		(void)(*server)->drain(
			http::DrainOptions{
				.deadline = 30s,
				.stop_accepting = true,
				.close_idle = true,
				.finish_requests = true,
				.finish_streams = false,
				.sse_policy = http::DrainStreamPolicy::close_with_retry,
			});
	}};

	return static_cast<int>((*server)->run());
}
