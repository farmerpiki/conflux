import conflux.http;

int main() {
	static_assert(requires { typename conflux::http::Router; }, "conflux_http_facade_unexpected_router_alias_visible");
}
