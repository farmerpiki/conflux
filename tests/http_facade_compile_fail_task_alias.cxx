// Intentionally invalid: Task shorthand is extended-only.
import conflux.http;

int main() {
	namespace http = conflux::http;
	static_assert(
		requires { typename http::Task<http::Response>; },
		"conflux_http_facade_unexpected_task_alias_visible");
}
