import conflux;
import std;

// NOLINTNEXTLINE(bugprone-exception-escape) -- quickstart setup can allocate before the noexcept run boundary.
int main() noexcept {
	namespace http = conflux::http;

	auto app = http::app();
	app.use(http::request_id());

	app.get("/", [] { return http::text("quickstart middleware\n"); });
	app.get("/request-id", [](http::RequestId request_id) {
		return http::text(std::format("request_id={}\n", request_id.get()));
	});

	return http::exit_code(std::move(app).run({.port = 9094}));
}
