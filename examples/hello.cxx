// Small example app: first-contact HTTP facade.
// Build and run: build/debug-clang-libcxx/conflux_hello
// Then: curl http://localhost:9090/
//       curl http://localhost:9090/hello/World
//       curl http://localhost:9090/api/ping
import conflux;
import std;

struct StatusReply {
	std::string status;
	std::string server;
};

template<>
struct conflux::json::JsonMembers<StatusReply> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("status", &StatusReply::status),
			conflux::json::json_member("server", &StatusReply::server),
		};
	}
	static constexpr std::string_view type_name() { return "StatusReply"; }
};

int main() {
	namespace http = conflux::http;
	auto app = http::app();

	app.get("/", [] {
		return http::html(
			"<html><body>"
			"<h1>conflux example</h1>"
			"<ul>"
			"<li><a href='/hello/World'>/hello/{name}</a></li>"
			"<li><a href='/api/ping'>/api/ping</a></li>"
			"</ul>"
			"</body></html>");
	});

	app.get("/hello/{name}", [](http::Path<"name"> name) {
		return http::html(std::format("<html><body><h1>Hello, {}!</h1></body></html>", name.get()));
	});

	app.get("/api/ping", [] { return http::json(StatusReply{.status = "ok", .server = "conflux"}); });

	auto const status = http::run(std::move(app), {.port = 9090});
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
