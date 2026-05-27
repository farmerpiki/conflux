// Intentionally invalid: HTTP reject reasons live in conflux::http.
import conflux.http;

auto probe() -> ::HttpRejectReason {
	return {};
}
