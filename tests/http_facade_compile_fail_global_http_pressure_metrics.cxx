// Intentionally invalid: HTTP pressure metrics live in conflux::http.
import conflux.http;

auto probe() -> ::HttpPressureMetrics * {
	return nullptr;
}
