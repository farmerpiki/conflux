// Gzip middleware example: compress responses for clients that Accept-Encoding: gzip.
// Build and run: build/debug-gcc-stdcxx/conflux_gzip
// Then:
//   curl -si -H "Accept-Encoding: gzip" http://localhost:9093/ | head -8
//   curl -s  -H "Accept-Encoding: gzip" http://localhost:9093/     | gunzip
//   curl -s                              http://localhost:9093/api/data  # uncompressed
import conflux.net.http;
import std;

int main() {
	Config cfg{};
	cfg.port = 9093;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;
	router.use(compress_middleware());

	router.get("/", [](HttpRequestView const &) {
		return HttpResponse::html(
			"<html><body>"
			"<h1>Gzip middleware example</h1>"
			"<p>This response is transparently gzip-compressed when the client "
			"sends <code>Accept-Encoding: gzip</code>.</p>"
			"<p>Try: <code>curl -H 'Accept-Encoding: gzip' http://localhost:9093/ | gunzip</code></p>"
			"</body></html>");
	});

	router.get("/api/data", [](HttpRequestView const &) {
		return HttpResponse::json(
			R"({"message":"JSON is also compressed when the client accepts gzip","items":[1,2,3]})");
	});

	HttpServer srv{cfg, std::move(router)};
	srv.run();
}
