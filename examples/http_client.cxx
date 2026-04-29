// HTTP client example.
//
// Run a local server in another shell, for example:
//   build/debug-clang-libcxx/conflux_hello
//
// Then:
//   build/debug-clang-libcxx/conflux_http_client
import conflux.net.http;
import std;
import conflux.types;

namespace http = conflux::http;

int main() {
	http::HttpClient client;

	auto r1 = client.send_blocking(http::HttpRequest::get("http://127.0.0.1:9090/").build());
	if (r1) {
		println("GET / -> {} {}", r1->head.status, r1->head.status_text);
		println("content-type: {}", r1->head.headers["content-type"]);
		println("body:\n{}", r1->body);
	} else {
		println(cerr, "GET failed: {}", r1.error().message);
	}

	auto r2 = client.send_blocking(
		http::HttpRequest::get("http://127.0.0.1:9090/api/ping").header("Accept", "application/json").build());
	if (r2) {
		println("GET /api/ping -> {} {}", r2->head.status, r2->body);
	} else {
		println(cerr, "GET /api/ping failed: {}", r2.error().message);
	}
}
