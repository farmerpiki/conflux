// Middleware-focused example.
// Shows request IDs, tracing, auth, and request-view handlers.
//
// Build and run: build/debug-gcc-stdcxx/conflux_middleware
// Try:
//   curl http://localhost:9094/
//   curl http://localhost:9094/public/ping
//   curl -u demo:demo http://localhost:9094/private/profile
//   curl -H 'Authorization: Bearer valid-token' http://localhost:9094/private/token
import conflux.net.http;
import std;
import conflux.types;

int main() {
	Config cfg{};
	cfg.port = 9094;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;

	router.use(request_id_middleware());
	router.use(tracing_middleware({.propagate_in_response = true}));

	router.get("/", [](HttpRequestView const &) {
		return HttpResponse::html(
			"<html><body>"
			"<h1>conflux middleware example</h1>"
			"<ul>"
			"<li><a href='/public/ping'>/public/ping</a></li>"
			"<li><a href='/private/profile'>/private/profile</a> (basic auth: demo/demo)</li>"
			"<li><a href='/private/token'>/private/token</a> (bearer token: valid-token)</li>"
			"</ul>"
			"</body></html>");
	});

	router.group("/public", [](Router::Group &g) {
		g.get("/ping", [](HttpRequestView const &req) {
			return HttpResponse::json(
				std::format(
					R"({{"status":"ok","request_id":"{}","traceparent":"{}"}})",
					req.headers["x-request-id"],
					req.headers["traceparent"]));
		});
	});

	router.group("/private", [](Router::Group &g) {
		g.use(basic_auth_middleware(
			[](SV user, SV pass) { return user == "demo" && pass == "demo"; }));

		g.get("/profile", [](HttpRequestView const &req) {
			return HttpResponse::json(
				std::format(
					R"({{"user":"demo","request_id":"{}","remote_addr":"{}"}})",
					req.headers["x-request-id"],
					req.remote_addr));
		});
	});

	router.group("/private", [](Router::Group &g) {
		g.use(bearer_auth_middleware([](SV token) { return token == "valid-token"; }));

		g.get("/token", [](HttpRequestView const &req) {
			return HttpResponse::json(
				std::format(R"({{"token":"accepted","request_id":"{}"}})", req.headers["x-request-id"]));
		});
	});

	HttpServer srv{cfg, std::move(router)};
	srv.run();
}
