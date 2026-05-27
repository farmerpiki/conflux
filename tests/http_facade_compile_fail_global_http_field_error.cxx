// Intentionally invalid: HTTP field error lives in conflux::http.
import conflux.http;

auto probe() -> ::HttpFieldError * {
	return nullptr;
}
