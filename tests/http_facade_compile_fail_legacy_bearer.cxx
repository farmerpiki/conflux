// Intentionally invalid: bearer extractors are presence-token types.
import conflux.http;

auto probe() -> conflux::http::RequiredBearer {
	return {};
}
