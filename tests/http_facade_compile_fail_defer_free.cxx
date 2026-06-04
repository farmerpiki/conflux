// Intentionally invalid: raw defer helper is extended HTTP API.
import std;
import conflux.http;

int main() {
	namespace http = conflux::http;
	static_assert(
		requires { http::defer(nullptr, [] { return http::text("ok"); }); },
		"conflux_http_facade_unexpected_defer_helper_visible");
	return 0;
}
