import conflux.http;
import std;

int main() {
	namespace http = conflux::http;

	auto app = http::app();
	app.use(request_id_middleware());

	app.get("/", [] { return http::text("quickstart middleware\n"); });
	app.get("/request-id", [](http::Header<"x-request-id"> request_id) {
		return http::text(std::format("request_id={}\n", request_id.get()));
	});

	return http::run(std::move(app), {.port = 9094}) == http::RunStatus::stopped_normally ? 0 : 1;
}
