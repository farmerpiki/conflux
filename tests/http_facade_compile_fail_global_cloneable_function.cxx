// Intentionally invalid: cloneable functions live in conflux::http.
import conflux.http;

auto probe() -> ::CloneableFunction<void()> * {
	return nullptr;
}
