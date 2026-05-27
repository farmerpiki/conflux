// Intentionally invalid: openapi_handler is extended HTTP API.
#include <conflux/http.hxx>

int main() {
	auto app = conflux::http::app();
	namespace http = conflux::http;
	static_assert(
		requires { http::openapi_handler(app, "API", "1.0.0"); },
		"conflux_http_facade_unexpected_openapi_handler_visible");
}
