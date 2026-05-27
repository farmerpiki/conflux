// Intentionally invalid: HTTP field error kind lives in conflux::http.
import conflux.http;

auto probe() -> ::HttpFieldErrorKind {
	return {};
}
