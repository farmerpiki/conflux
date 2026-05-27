import conflux;
import std;

int main() {
	namespace http = conflux::http;

	auto app = http::app();

	app.get("/", [] { return http::text("hello from conflux\n"); });
	app.get<"/hello/{name}">(
		[](http::Path<"name"> name) { return http::html(std::format("<h1>Hello, {}!</h1>", name.get())); });

	return http::exit_code(std::move(app).run({.port = 9090}));
}
