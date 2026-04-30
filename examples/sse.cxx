// SSE example: demonstrates Server-Sent Events with named params.
// Build and run: build/debug-gcc-stdcxx/conflux_sse
// Then: curl -N http://localhost:9091/events
//       curl -N http://localhost:9091/events/alice
import conflux.net.http;
import std;

int main() {
	namespace http = conflux::http;
	auto app = http::App::default_server();

	// Raw frames, 200 ms apart.
	app.sse("/events", [](http::Request const &, SP<SseChannel> const &ch) {
		for (int i = 1; i <= 5; ++i) {
			ch->send(std::format("data: event{}\n\n", i));
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
		ch->close();
	});

	// Named events via send_event(), with path param capture.
	app.sse("/events/{name}", [](http::Request const &req, SP<SseChannel> const &ch) {
		auto name = req.params["name"];
		for (int i = 1; i <= 3; ++i) {
			ch->send_event("greet", std::format("hello {}, message {}", name, i));
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
		}
		ch->close();
	});

	std::move(app).run({.port = 9091});
}
