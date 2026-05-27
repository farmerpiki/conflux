// Intentionally invalid: HTTP field source lives in conflux::http.
import conflux.http;

auto probe() -> ::HttpFieldSource {
	return {};
}
