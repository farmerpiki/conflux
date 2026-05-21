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

static Router make_api_router() {
	Router api;
	api.use(request_id_middleware());
	api.get("/status", [](Request const &req) {
		return Response::json(std::format(R"({{"host":"api","request_id":"{}"}})", req.headers["x-request-id"]));
	});
	api.get("/users/{id}", [](Request const &req) {
		return Response::json(std::format(R"({{"id":"{}","name":"example"}})", req.params["id"]));
	});
	api.get("/openapi.json", openapi_handler(api, "api.local.test", "0.1.0"));
	return api;
}

static Router make_web_router() {
	Router web;
	web.get("/status", [](Request const &) { return Response::html("<h1>web ok</h1>"); });
	web.get("/", [](Request const &) {
		return Response::html("<html><body><h1>web host</h1><p>Try /status.</p></body></html>");
	});
	return web;
}

int main() {
	VHostRouter hosts;
	hosts.add("api.local.test", make_api_router());
	hosts.add("web.local.test", make_web_router());

	Router fallback;
	fallback.get("/status", [](Request const &req) {
		return Response::text(std::format("default host handler: {}\n", req.headers["host"]));
	});
	hosts.set_default(std::move(fallback));

	Config cfg = Config::low_latency();
	cfg.port = 9101;
	std::println("virtual-host server listening on http://localhost:9101/");
	HttpServer srv{cfg, std::move(hosts)};
	auto const status = srv.run();
	return status == RunStatus::stopped_normally ? 0 : 1;
}
