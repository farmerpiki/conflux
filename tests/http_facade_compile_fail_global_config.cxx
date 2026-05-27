// Intentionally invalid: HTTP config lives in conflux::http.
import conflux.http;

auto probe() -> ::Config * {
	return nullptr;
}
