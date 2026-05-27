// Intentionally invalid: offload helpers are extended HTTP API.
#include <conflux/http.hxx>

int main() {
	namespace http = conflux::http;
	static_assert(
		requires { http::offload(nullptr, [] { return http::text("ok"); }); },
		"conflux_http_facade_unexpected_offload_helper_visible");
}
