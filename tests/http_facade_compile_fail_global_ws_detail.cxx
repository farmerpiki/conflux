// Intentionally invalid: WebSocket internals are not global helpers.
import conflux.http;

auto probe() -> decltype(::ws_detail::ws_accept_key("key")) {
	return {};
}
