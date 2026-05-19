// Gzip middleware example: compress responses for clients that Accept-Encoding: gzip.
// Build and run: build/debug-clang-libcxx/conflux_gzip
// Then:
//   curl -si -H "Accept-Encoding: gzip" http://localhost:9093/ | head -8
//   curl -s  -H "Accept-Encoding: gzip" http://localhost:9093/     | gunzip
//   curl -s                              http://localhost:9093/api/data  # uncompressed
import conflux.http;
import std;
int main() {
	namespace http = conflux::http;

	auto app = http::app();
	app.use(compress_middleware());

	app.get("/", [](http::RequestView const &) {
		return http::html(
			"<html><body>"
			"<h1>Gzip middleware example</h1>"
			"<p>This response is transparently gzip-compressed when the client "
			"sends <code>Accept-Encoding: gzip</code>.</p>"
			"<p>Try: <code>curl -H 'Accept-Encoding: gzip' http://localhost:9093/ | gunzip</code></p>"
			"</body></html>");
	});

	app.get("/api/data", [](http::RequestView const &) {
		return http::json_response(
			R"({"message":"JSON is also compressed when the client accepts gzip","items":[1,2,3]})");
	});

	auto const status = http::run(std::move(app), {.port = 9093});
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
