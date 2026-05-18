// HTTP observability example: metrics + request IDs + security headers.
//
// Build and run: build/debug-gcc-stdcxx/conflux_http_observability_example
// Try:
//   curl -i http://localhost:9095/health
//   curl -i http://localhost:9095/slow
//   curl -H 'Authorization: Bearer metrics-token' http://localhost:9095/metrics
import conflux.net.http.server;
import conflux.types;
import std;

int main() {
	namespace http = conflux::http;

	MetricsRegistry metrics;
	auto app = http::App::default_server();

	app.use(request_id_middleware());
	app.use(metrics_middleware(metrics));
	app.use(security_headers_middleware({
		.hsts_max_age = 0,
		.csp = "default-src 'none'; frame-ancestors 'none'",
	}));
	app.use(cache_control_middleware({.default_directive = "no-store"}));
	app.use(rate_limit_middleware({.requests = 60, .window = chrono::seconds{60}, .burst = 10}));

	app.get("/", [](HttpRequest const &) {
		return HttpResponse::html(
			"<html><body>"
			"<h1>conflux observability example</h1>"
			"<ul>"
			"<li><a href='/health'>/health</a></li>"
			"<li><a href='/slow'>/slow</a></li>"
			"<li>/metrics requires: Authorization: Bearer metrics-token</li>"
			"</ul>"
			"</body></html>");
	});

	app.get("/health", [](HttpRequest const &req) {
		return HttpResponse::text(format(
			"ok\nrequest_id={}\nremote={}\n",
			req.headers["x-request-id"],
			req.remote_addr));
	});

	app.get("/slow", [](HttpRequest const &req) {
		std::this_thread::sleep_for(chrono::milliseconds{50});
		return HttpResponse::text(format("slow path completed\nrequest_id={}\n", req.headers["x-request-id"]));
	});

	std::vector<Router::Middleware> metrics_auth;
	metrics_auth.push_back(bearer_auth_middleware([](std::string_view token) { return token == "metrics-token"; }));
	app.get("/metrics", metrics_handler_protected(metrics, move(metrics_auth)));

	auto const status = move(app).run({.port = 9095});
	return status == RunStatus::stopped_normally ? 0 : 1;
}
