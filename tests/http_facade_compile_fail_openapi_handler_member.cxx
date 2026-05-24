// Compile-fail guard: OpenAPI handler generation is extended API, not a curated App member.
import conflux.http;

template<class App>
concept has_openapi_handler_member = requires(App app) { app.openapi_handler("API", "1.0.0"); };

int main() {
	static_assert(
		has_openapi_handler_member<conflux::http::App>,
		"conflux_http_facade_unexpected_openapi_handler_member_visible");
}
