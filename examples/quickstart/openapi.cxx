import conflux.http;
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

	app.get("/health", [] { return http::Json{StatusReply{.status = "ok"}}; })
		.name("health.check")
		.openapi_summary("Health check");
	app.get("/openapi.json", app.openapi_handler("conflux quickstart", "0.1.0"));

	return http::run(std::move(app), {.port = 9098}) == http::RunStatus::stopped_normally ? 0 : 1;
}
