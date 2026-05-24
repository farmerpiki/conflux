// Intentionally invalid: openapi_handler is extended HTTP API.
import std;
import conflux.http;

int main() {
	namespace http = conflux::http;
	auto app = http::app();
	static_assert(
		requires { http::openapi_handler(app, "API", "1.0.0"); },
		"conflux_http_facade_unexpected_openapi_handler_visible");
}
