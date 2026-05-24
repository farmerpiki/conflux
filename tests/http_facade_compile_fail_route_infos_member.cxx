import conflux.http;

template<class App>
concept has_route_infos_member = requires(App app) { app.route_infos(); };

int main() {
	static_assert(
		has_route_infos_member<conflux::http::App>,
		"conflux_http_facade_unexpected_route_infos_member_visible");
}
