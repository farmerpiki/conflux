// Intentionally invalid: request context vocabulary lives in conflux::http.
import conflux.http;

auto probe() -> RequestContext {
	return {};
}
