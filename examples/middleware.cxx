// Middleware-focused example.
// Shows request IDs, tracing, auth, and request handlers.
//
// Build and run: build/debug-clang-libcxx/conflux_middleware
// Try:
//   curl http://localhost:9094/
//   curl http://localhost:9094/public/ping
//   curl -u demo:demo http://localhost:9094/private/profile
//   curl -H 'Authorization: Bearer valid-token' http://localhost:9094/private/token
import conflux.http;
import std;
int main() {
	namespace http = conflux::http;
	auto app = http::app();

	app.use(request_id_middleware());
	app.use(tracing_middleware({.propagate_in_response = true}));

	app.get("/", [](http::Request const &) {
		return http::html(
			"<html><body>"
			"<h1>conflux middleware example</h1>"
			"<ul>"
			"<li><a href='/public/ping'>/public/ping</a></li>"
			"<li><a href='/private/profile'>/private/profile</a> (basic auth: demo/demo)</li>"
			"<li><a href='/private/token'>/private/token</a> (bearer token: valid-token)</li>"
			"</ul>"
			"</body></html>");
	});

	app.group("/public", [](http::Router::Group &g) {
		g.get("/ping", [](http::Request const &req) {
			return http::json_response(
				std::format(
					R"({{"status":"ok","request_id":"{}","traceparent":"{}"}})",
					req.header("x-request-id"),
					req.header("traceparent")));
		});
	});

	app.group("/private", [](http::Router::Group &g) {
		g.use(basic_auth_middleware(
			[](std::string_view user, std::string_view pass) { return user == "demo" && pass == "demo"; }));

		g.get("/profile", [](http::Request const &req) {
			return http::json_response(
				std::format(
					R"({{"user":"demo","request_id":"{}","remote_addr":"{}"}})",
					req.header("x-request-id"),
					req.remote_addr));
		});
	});

	app.group("/private", [](http::Router::Group &g) {
		g.use(bearer_auth_middleware([](std::string_view token) { return token == "valid-token"; }));

		g.get("/token", [](http::Request const &req) {
			return http::json_response(
				std::format(R"({{"token":"accepted","request_id":"{}"}})", req.header("x-request-id")));
		});
	});

	auto const status = http::run(std::move(app), {.port = 9094});
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
