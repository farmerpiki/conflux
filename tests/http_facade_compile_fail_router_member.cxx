import conflux.http;

template<class App>
concept has_router_member = requires(App app) { app.router(); };

int main() {
	static_assert(has_router_member<conflux::http::App>, "conflux_http_facade_unexpected_router_member_visible");
}
