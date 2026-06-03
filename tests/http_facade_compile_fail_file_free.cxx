// Intentionally invalid: file helper is extended HTTP API.
import std;
import conflux.http;

int main() {
	namespace http = conflux::http;
	static_assert(requires { http::file("index.html"); }, "conflux_http_facade_unexpected_file_helper_visible");
}
