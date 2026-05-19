import conflux.http;
import std;

int main() {
	namespace http = conflux::http;

	auto app = http::app();

	app.get("/", [] { return http::text("hello from conflux\n"); });
	app.get<"/hello/{name}">(
		[](http::Path<"name"> name) { return http::html(std::format("<h1>Hello, {}!</h1>", name.get())); });

	return http::run(std::move(app), {.port = 9090}) == http::RunStatus::stopped_normally ? 0 : 1;
}
