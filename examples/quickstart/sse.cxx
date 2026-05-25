import conflux;
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

	return http::run_main(std::move(app), {.port = 9091});
}
