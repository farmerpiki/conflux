#include <conflux/http.hxx>

int main() {
	namespace http = conflux::http;
	static_assert(
		requires { typename http::Task<http::Response>; },
		"conflux_http_facade_unexpected_task_alias_visible");
}
