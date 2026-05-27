// Intentionally invalid: HTTP drain reports live in conflux::http.
import conflux.http;

auto probe() -> ::DrainReport * {
	return nullptr;
}
