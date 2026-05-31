import conflux.net.config;
import conflux.net.jwt;

auto probe(
	conflux::http::Config const &cfg) {
	return ::jwt_options_from_config(cfg);
}
