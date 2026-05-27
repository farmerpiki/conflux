// Intentionally invalid: parser limits live in conflux::http.
import conflux.http;

auto probe() -> ::ParserLimits * {
	return nullptr;
}
