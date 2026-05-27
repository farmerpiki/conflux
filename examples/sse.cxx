// SSE example: demonstrates Server-Sent Events with named params.
// Build and run: build/debug-clang-libcxx/conflux_sse
// Then: curl -N http://localhost:9091/events
//       curl -N http://localhost:9091/events/alice
import conflux;
import std;
int main() {
	namespace http = conflux::http;
	auto app = http::app();

	// Raw frames.
	app.sse("/events", [](http::RequestView const &, std::shared_ptr<http::SseChannel> const &ch) {
		for (int i = 1; i <= 5; ++i) {
			auto _ = ch->send(std::format("data: event{}\n\n", i));
		}
		ch->close();
	});

	// Named events via send_event(), with path param capture.
	app.sse("/events/{name}", [](http::RequestView const &req, std::shared_ptr<http::SseChannel> const &ch) {
		auto name = req.param("name");
		for (int i = 1; i <= 3; ++i) {
			auto _ = ch->send_event("greet", std::format("hello {}, message {}", name, i));
		}
		ch->close();
	});

	auto const status = std::move(app).run({.port = 9091});
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
