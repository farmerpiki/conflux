// Intentionally invalid: SSE channel helpers live in conflux::http.
import conflux.http;

auto probe() -> ::SseChannel * {
	return nullptr;
}
