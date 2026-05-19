import conflux.http;
import std;

struct StatusReply {
	std::string status;
	std::string server;
};

template<>
struct JsonMembers<StatusReply> {
	static constexpr auto members() {
		return std::tuple{
			json_member("status", &StatusReply::status),
			json_member("server", &StatusReply::server),
		};
	}
	static constexpr std::string_view type_name() { return "StatusReply"; }
};

int main() {
	namespace http = conflux::http;

	auto app = http::app();

	app.get("/", [] { return http::text("hello from conflux\n"); });
	app.get<"/hello/{name}">(
		[](http::Path<"name"> name) { return http::html(std::format("<h1>Hello, {}!</h1>", name.get())); });
	app.get("/api/ping", [] {
		return http::Json{
			StatusReply{.status = "ok", .server = "conflux"}
        };
	});

	return http::run(std::move(app), {.port = 9090}) == http::RunStatus::stopped_normally ? 0 : 1;
}
