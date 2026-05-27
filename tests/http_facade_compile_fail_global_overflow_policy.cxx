// Intentionally invalid: HTTP overflow policy lives in conflux::http.
import conflux.http;

auto probe() -> ::OverflowPolicy {
	return {};
}
