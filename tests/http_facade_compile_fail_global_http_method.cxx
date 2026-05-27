// Intentionally invalid: HTTP method vocabulary lives in conflux::http.
import conflux.http;

auto probe() -> HttpMethod {
	return HttpMethod::get;
}
