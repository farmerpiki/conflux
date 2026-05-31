import conflux.net.config;
import conflux.net.cookie_signing;

auto probe(
	conflux::http::Config const &config) {
	return ::cookie_signing_options_from_config(config);
}
