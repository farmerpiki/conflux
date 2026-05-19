// SSE example: demonstrates Server-Sent Events with named params.
// Build and run: build/debug-clang-libcxx/conflux_sse
// Then: curl -N http://localhost:9091/events
//       curl -N http://localhost:9091/events/alice
import conflux.http;
import std;
int main() {
	namespace http = conflux::http;
	auto app = http::app();

	// Raw frames, 200 ms apart.
	app.sse("/events", [](http::Request const &, std::shared_ptr<http::SseChannel> const &ch) {
		for (int i = 1; i <= 5; ++i) {
			auto _ = ch->send(std::format("data: event{}\n\n", i));
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
		ch->close();
	});

	// Named events via send_event(), with path param capture.
	app.sse("/events/{name}", [](http::Request const &req, std::shared_ptr<http::SseChannel> const &ch) {
		auto name = req.param("name");
		for (int i = 1; i <= 3; ++i) {
			auto _ = ch->send_event("greet", std::format("hello {}, message {}", name, i));
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
		}
		ch->close();
	});

	auto const status = http::run(std::move(app), {.port = 9091});
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
