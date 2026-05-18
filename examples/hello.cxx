// Small example app: easy HTTP facade over Router + HttpServer defaults.
// Demonstrates the three preferred handler shapes:
//   - sync: HttpResponse
//   - root async: root::Task<HttpResponse>
//   - deferred pool offload: http::defer(...)
// Build and run: build/debug-gcc-stdcxx/examples/hello
// Then: curl http://localhost:9090/
//       curl http://localhost:9090/hello/World
//       curl http://localhost:9090/api/ping
//       curl http://localhost:9090/api/defer-ping
import conflux.net.http.server;
import conflux.work;
import conflux.work.root;
import std;
int main() {
	namespace http = conflux::http;
	auto app = http::App::default_server();

	app.get("/", [](HttpRequest const &) {
		return HttpResponse::html(
			"<html><body>"
			"<h1>conflux example</h1>"
			"<ul>"
			"<li><a href='/hello/World'>/hello/{name}</a></li>"
			"<li><a href='/api/ping'>/api/ping</a></li>"
			"</ul>"
			"</body></html>");
	});

	app.get("/hello/{name}", [](HttpRequest const &req) {
		return HttpResponse::html(format("<html><body><h1>Hello, {}!</h1></body></html>", req.params["name"]));
	});

	app.get("/api/ping", [](HttpRequest const &) {
		return HttpResponse::json(R"({"status":"ok","server":"conflux"})");
	});

	app.get("/api/async-ping", [](HttpRequest const &) -> conflux::work::root::Task<HttpResponse> {
		auto [task, source] = conflux::work::root::make_task_source<HttpResponse>();
		auto _ = source.try_set_value(
			conflux::work::root::Success<HttpResponse>{HttpResponse::json(R"({"status":"ok","mode":"async"})")});
		return move(task);
	});

	app.get("/api/defer-ping", [](HttpRequest const &) {
		static auto pool = make_shared<WorkPool>(WorkPoolOptions{.threads = 1, .max_inject_queue = 16});
		return http::defer(pool, [] { return HttpResponse::json(R"({"status":"ok","mode":"defer"})"); });
	});

	auto const status = move(app).run({.port = 9090});
	return status == RunStatus::stopped_normally ? 0 : 1;
}
