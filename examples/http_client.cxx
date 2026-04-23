// HTTP client example.
//
// Run a local server in another shell, for example:
//   build/debug-gcc-stdcxx/conflux_hello
//
// Then:
//   build/debug-gcc-stdcxx/conflux_http_client
import conflux.net.http;
import std;

int main() {
	ClientOptions options;
	options.host = "127.0.0.1";
	options.port = 9090;
	options.timeout_sec = 5;
	HttpClient client{std::move(options)};

	if (auto response = client.get("/"); response) {
		std::println("GET / -> {} {}", response->status, response->status_text);
		std::println("content-type: {}", response->content_type);
		std::println("body:\n{}", response->body);
	} else {
		std::println(std::cerr, "GET failed: {}", response.error());
	}

	HttpFields headers{true};
	headers["Accept"] = "application/json";
	if (auto response = client.get("/api/ping", headers); response) {
		std::println("GET /api/ping -> {} {}", response->status, response->body);
	} else {
		std::println(std::cerr, "GET /api/ping failed: {}", response.error());
	}
}
