// Intentionally invalid: cookie response helpers live in conflux::http.
import conflux.http;

auto probe() -> CookieBuilder {
	return {"session", "abc"};
}
