// Virtual-host example: dispatch by Host header, with per-host routers and
// generated OpenAPI specs on each API surface.
//
// Build and run: build/debug-gcc-stdcxx/conflux_vhost_openapi_example
// Try:
//   curl -H 'Host: api.local.test' http://localhost:9101/status
//   curl -H 'Host: web.local.test' http://localhost:9101/status
//   curl -H 'Host: unknown.local.test' http://localhost:9101/status
//   curl -H 'Host: api.local.test' http://localhost:9101/openapi.json
import conflux.net.http;
import conflux.types;
import std;

static Router make_api_router() {
	Router api;
	api.use(request_id_middleware());
	api.get("/status", [](HttpRequest const &req) {
		return HttpResponse::json(format(
			R"({{"host":"api","request_id":"{}"}})",
			req.headers["x-request-id"]));
	});
	api.get("/users/{id}", [](HttpRequest const &req) {
		return HttpResponse::json(format(
			R"({{"id":"{}","name":"example"}})",
			req.params["id"]));
	});
	api.get("/openapi.json", openapi_handler(api, "api.local.test", "0.1.0"));
	return api;
}

static Router make_web_router() {
	Router web;
	web.get("/status", [](HttpRequest const &) { return HttpResponse::html("<h1>web ok</h1>"); });
	web.get("/", [](HttpRequest const &) {
		return HttpResponse::html(
			"<html><body><h1>web host</h1><p>Try /status.</p></body></html>");
	});
	return web;
}

int main() {
	VHostRouter hosts;
	hosts.add("api.local.test", make_api_router());
	hosts.add("web.local.test", make_web_router());

	Router fallback;
	fallback.get("/status", [](HttpRequest const &req) {
		return HttpResponse::text(format("default host handler: {}\n", req.headers["host"]));
	});
	hosts.set_default(move(fallback));

	Config cfg = Config::low_latency();
	cfg.port = 9101;
	std::println("virtual-host server listening on http://localhost:9101/");
	HttpServer srv{cfg, move(hosts)};
	auto const status = srv.run();
	return status == RunStatus::stopped_normally ? 0 : 1;
}
