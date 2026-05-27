// Intentionally invalid: HTTP send-zc metrics live in conflux::http.
import conflux.http;

auto probe() -> ::SendZcMetrics * {
	return nullptr;
}
