// Intentionally invalid: HTTP server metrics live in conflux::http.
import conflux.http;

auto probe() -> ::HttpServerMetrics * {
	return nullptr;
}
