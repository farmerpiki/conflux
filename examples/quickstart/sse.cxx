import conflux.http;
import std;

int main() {
	namespace http = conflux::http;

	auto app = http::app();
	app.get("/", [] { return http::text("stream /events\n"); });
	app.sse("/events", [](http::RequestView const &, std::shared_ptr<http::SseChannel> const &channel) {
		for (int i = 1; i <= 3; ++i) {
			auto _ = channel->send_event("message", std::format("event {}", i));
		}
		channel->close();
	});

	return static_cast<int>(std::move(app).run({.port = 9091}));
}
