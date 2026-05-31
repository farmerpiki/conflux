import conflux.net.metrics;
import std;

auto probe(
	conflux::http::MetricsRegistry const &registry) {
	return ::metrics_handler_protected(registry, std::vector<conflux::http::Router::Middleware>{});
}
