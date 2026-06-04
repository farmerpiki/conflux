// Intentionally invalid: AsyncNext shorthand is extended-only.
import conflux.http;

int main() {
	namespace http = conflux::http;
	static_assert(requires { typename http::AsyncNext; }, "conflux_http_facade_unexpected_async_next_alias_visible");
}
