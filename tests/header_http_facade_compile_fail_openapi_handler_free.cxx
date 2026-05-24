// Intentionally invalid: openapi_handler is extended HTTP API.
#include <conflux/http.hxx>

int main() {
	auto app = conflux::http::app();
	(void)conflux::http::openapi_handler(app, "API", "1.0.0");
}
