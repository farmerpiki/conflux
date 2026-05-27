// Intentionally invalid: HTTP drain options live in conflux::http.
import conflux.http;

auto probe() -> ::DrainOptions * {
	return nullptr;
}
