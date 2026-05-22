import conflux.http;
import std;

int main() {
	namespace http = conflux::http;

	auto app = http::app();
	app.use(request_id_middleware());

	app.get("/", [] { return http::text("quickstart middleware\n"); });
	app.get("/request-id", [](http::RequestId request_id) {
		return http::text(std::format("request_id={}\n", request_id.get()));
	});

	return static_cast<int>(std::move(app).run({.port = 9094}));
}
