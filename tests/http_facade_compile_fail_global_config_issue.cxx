// Intentionally invalid: config issue diagnostics live in conflux::http.
import conflux.http;

auto probe() -> ::ConfigIssue * {
	return nullptr;
}
