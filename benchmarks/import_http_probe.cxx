import std;
import conflux.http;

namespace http = conflux::http;

int main(
	int argc,
	char **argv) {
	for (int i = 1; i < argc; ++i) {
		if (std::string_view{argv[i]} == "--bench-info") {
			std::println("{{\"name\":\"import_http_probe\",\"parser\":\"compile-only\",\"configs\":[]}}");
			return 0;
		}
	}
	auto app = http::app();
	app.get("/", [] { return http::text("ok"); });
	return app.routes().empty() ? 1 : 0;
}
