// HTTP observability example: unified request IDs, tracing, logs, and metrics.
//
// Build and run: build/release-clang-libcxx/conflux_http_observability_example
// Try:
//   curl -i http://localhost:9095/health
//   curl -i http://localhost:9095/slow
//   curl -i http://localhost:9095/metrics
import conflux.http;
import std;

int main() {
	namespace http = conflux::http;

	auto app = http::app();

	app.use(
		http::observability({
			.service_name = "observability-example",
			.metrics_path = "/metrics",
		}));
	app.use(security_headers_middleware({
		.hsts_max_age = 0,
		.csp = "default-src 'none'; frame-ancestors 'none'",
	}));
	app.use(cache_control_middleware({.default_directive = "no-store"}));
	app.use(rate_limit_middleware({.requests = 60, .window = std::chrono::seconds{60}, .burst = 10}));

	app.get("/", [](http::Request const &) {
		return http::html(
			"<html><body>"
			"<h1>conflux observability example</h1>"
			"<ul>"
			"<li><a href='/health'>/health</a></li>"
			"<li><a href='/slow'>/slow</a></li>"
			"<li>/metrics requires: Authorization: Bearer metrics-token</li>"
			"</ul>"
			"</body></html>");
	});

	app.get("/health", [](http::RequestId request_id, http::ConnectionInfo conn) {
		return http::text(std::format("ok\nrequest_id={}\nremote={}\n", request_id.get(), conn.remote_addr));
	});

	app.get("/slow", [](http::RequestId request_id) {
		std::this_thread::sleep_for(std::chrono::milliseconds{50});
		return http::text(std::format("slow path completed\nrequest_id={}\n", request_id.get()));
	});

	auto const status = http::run(std::move(app), {.port = 9095});
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
