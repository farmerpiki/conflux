// SSE example: demonstrates Server-Sent Events with named params.
// Build and run: build/debug-gcc-stdcxx/conflux_sse
// Then: curl -N http://localhost:9091/events
//       curl -N http://localhost:9091/events/alice
import conflux.net.http;
import std;
import conflux.types;

int main() {
	Config cfg{};
	cfg.port = 9091;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;

	// Raw frames, 200 ms apart.
	router.sse("/events", [](HttpRequestView const &, SP<SseChannel> const &ch) {
		for (int i = 1; i <= 5; ++i) {
			ch->send(std::format("data: event{}\n\n", i));
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
		ch->close();
	});

	// Named events via send_event(), with path param capture.
	router.sse("/events/{name}", [](HttpRequestView const &req, SP<SseChannel> const &ch) {
		auto name = req.params["name"];
		for (int i = 1; i <= 3; ++i) {
			ch->send_event("greet", std::format("hello {}, message {}", name, i));
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
		}
		ch->close();
	});

	HttpServer srv{cfg, std::move(router)};
	srv.run();
}
