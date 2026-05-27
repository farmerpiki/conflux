// Intentionally invalid: HTTP rejection metrics live in conflux::http.
import conflux.http;

auto probe() -> ::HttpRejectionMetrics * {
	return nullptr;
}
