// Intentionally invalid: HTTP drain stream policy lives in conflux::http.
import conflux.http;

auto probe() -> ::DrainStreamPolicy {
	return {};
}
