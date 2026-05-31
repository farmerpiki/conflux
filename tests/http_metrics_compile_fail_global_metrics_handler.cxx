import conflux.net.metrics;

auto probe(
	conflux::http::MetricsRegistry const &registry) {
	return ::metrics_handler(registry);
}
