// Intentionally invalid: WebSocket upgrade helpers live in conflux::http.
import conflux.http;

auto probe() -> ::WsUpgrade * {
	return nullptr;
}
