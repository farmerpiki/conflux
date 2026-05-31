import conflux.net.metrics;

auto probe(
	conflux::http::MetricsRegistry &registry) {
	return ::metrics_middleware(registry);
}
