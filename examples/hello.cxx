// Small example app: demonstrates conflux router + io_uring HTTP server.
// Build and run: build/debug-gcc-stdcxx/examples/hello
// Then: curl http://localhost:9090/
//       curl http://localhost:9090/hello/World
//       curl http://localhost:9090/api/ping
import conflux.net.http;
import std;

int main() {
	Config cfg{};
	cfg.port = 9090;
	cfg.rings = 0; // 0 → hardware_concurrency
	cfg.ring_entries = 1024;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;

	router.get("/", [](HttpRequestView const &) {
		return HttpResponse::html(
			"<html><body>"
			"<h1>conflux example</h1>"
			"<ul>"
			"<li><a href='/hello/World'>/hello/{name}</a></li>"
			"<li><a href='/api/ping'>/api/ping</a></li>"
			"</ul>"
			"</body></html>");
	});

	router.get("/hello/{name}", [](HttpRequestView const &req) {
		return HttpResponse::html(std::format("<html><body><h1>Hello, {}!</h1></body></html>", req.params["name"]));
	});

	router.get("/api/ping", [](HttpRequestView const &) {
		return HttpResponse::json(R"({"status":"ok","server":"conflux"})");
	});

	HttpServer srv{cfg, std::move(router)};
	srv.run();
}
