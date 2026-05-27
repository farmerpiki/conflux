// Intentionally invalid: file helper is extended HTTP API.
#include <conflux/http.hxx>

int main() {
	namespace http = conflux::http;
	static_assert(requires { http::file("index.html"); }, "conflux_http_facade_unexpected_file_helper_visible");
}
