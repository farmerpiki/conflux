// Virtual-host example: dispatch by Host header, with per-host routers and
// generated OpenAPI specs on each API surface.
//
// Build and run: build/release-clang-libcxx/conflux_vhost_openapi_example
// Try:
//   curl -H 'Host: api.local.test' http://localhost:9101/status
//   curl -H 'Host: web.local.test' http://localhost:9101/status
//   curl -H 'Host: unknown.local.test' http://localhost:9101/status
//   curl -H 'Host: api.local.test' http://localhost:9101/openapi.json
import conflux.net.http.server;
import conflux.types;
import std;

static conflux::http::Router make_api_router() {
	conflux::http::Router api;
	api.use(conflux::http::request_id_middleware());
	api.get("/status", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::json(
			std::format(R"({{"host":"api","request_id":"{}"}})", req.headers["x-request-id"]));
	});
	api.get("/users/{id}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::json(std::format(R"({{"id":"{}","name":"example"}})", req.params["id"]));
	});
	api.get("/openapi.json", conflux::http::openapi_handler(api, "api.local.test", "0.1.0"));
	return api;
}

static conflux::http::Router make_web_router() {
	conflux::http::Router web;
	web.get("/status", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::html("<h1>web ok</h1>"); });
	web.get("/", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::html("<html><body><h1>web host</h1><p>Try /status.</p></body></html>");
	});
	return web;
}

int main() {
	conflux::http::VHostRouter hosts;
	hosts.add("api.local.test", make_api_router());
	hosts.add("web.local.test", make_web_router());

	conflux::http::Router fallback;
	fallback.get("/status", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::format("default host handler: {}\n", req.headers["host"]));
	});
	hosts.set_default(std::move(fallback));

	conflux::http::Config cfg = conflux::http::Config::low_latency();
	cfg.port = 9101;
	std::println("virtual-host server listening on http://localhost:9101/");
	conflux::http::HttpServer srv{cfg, std::move(hosts)};
	auto const status = srv.run();
	return status == conflux::http::RunStatus::stopped_normally ? 0 : 1;
}
