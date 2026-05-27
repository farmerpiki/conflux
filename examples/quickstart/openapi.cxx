import conflux;
import std;

struct StatusReply {
	std::string status;
};

template<>
struct conflux::json::JsonMembers<StatusReply> {
	static constexpr auto members() { return std::tuple{conflux::json::json_member("status", &StatusReply::status)}; }
	static constexpr std::string_view type_name() { return "StatusReply"; }
};

int main() {
	namespace http = conflux::http;

	auto app = http::app();

	app.get("/health", [] { return http::json(StatusReply{.status = "ok"}); })
		.name("health.check")
		.openapi_summary("Health check");
	(void)app.openapi("/openapi.json", "conflux quickstart", "0.1.0");

	return http::exit_code(std::move(app).run({.port = 9098}));
}
