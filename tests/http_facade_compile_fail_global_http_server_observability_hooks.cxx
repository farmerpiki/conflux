// Intentionally invalid: HTTP server observability hooks live in conflux::http.
import conflux.http;

auto probe() -> ::HttpServerObservabilityHooks * {
	return nullptr;
}
