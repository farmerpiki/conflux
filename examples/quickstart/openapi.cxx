import conflux;
import std;

struct StatusReply {
	std::string status;
};

template<>
struct JsonMembers<StatusReply> {
	static constexpr auto members() { return std::tuple{json_member("status", &StatusReply::status)}; }
	static constexpr std::string_view type_name() { return "StatusReply"; }
};

int main() {
	namespace http = conflux::http;

	auto app = http::app();

	app.get("/health", [] { return http::json(StatusReply{.status = "ok"}); })
		.name("health.check")
		.openapi_summary("Health check");
	app.get("/openapi.json", [&app] { return http::Response::json(app.openapi_spec("conflux quickstart", "0.1.0")); });

	return static_cast<int>(std::move(app).run({.port = 9098}));
}
