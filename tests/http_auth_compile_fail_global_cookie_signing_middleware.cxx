import conflux.net.cookie_signing;
import std;

auto probe(
	conflux::http::CookieSigningOptions options) {
	return ::cookie_signing_middleware(std::move(options));
}
