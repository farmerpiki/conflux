import conflux.net.metrics;

auto probe(
	conflux::http::HttpPressureMetrics const &pressure) {
	return ::format_pressure_metrics_prometheus(pressure);
}
