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
		.openapi_summary("Health check")
		.openapi_description("Returns current service health status.")
		.openapi_tags({"health"});
	(void)app.openapi("/openapi.json", "conflux quickstart", "0.1.0", {
		.description = "Conflux HTTP framework quickstart demo.",
		.contact_name = "conflux",
		.contact_url = "https://github.com/conflux-framework/conflux",
		.server_url = "http://localhost:9098",
	});

	return http::exit_code(std::move(app).run({.port = 9098}));
}
