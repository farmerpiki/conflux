import conflux.net.config;
import conflux.net.password_hash;

auto probe(
	conflux::http::Config const &cfg) {
	return ::password_hash_secrets_from_config(cfg);
}
