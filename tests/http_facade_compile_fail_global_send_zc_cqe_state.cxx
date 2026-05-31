// Intentionally invalid: HTTP send-zc CQE helpers live in conflux::http.
import conflux.http;

auto probe() -> ::SendZcCqeState * {
	return nullptr;
}
