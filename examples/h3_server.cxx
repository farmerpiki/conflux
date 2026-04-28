// Minimal HTTPS + HTTP/3 server.
//
// Run:
//   build/debug-gcc-stdcxx/conflux_h3_server
//
// Try:
//   curl -k https://localhost:9443/ping
//   curl -k --http3-only https://localhost:9443/ping
#include <cstdlib>

import conflux.net.http;
import std;

int main() {
	S cert_path = "/tmp/conflux_h3_server_cert.pem";
	S key_path = "/tmp/conflux_h3_server_key.pem";
	S const gen_cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_path,
		cert_path);
	if (std::system(gen_cmd.c_str()) != 0) {
		std::println(std::cerr, "openssl req failed");
		return 1;
	}

	Config cfg{};
	cfg.port = 9443;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.cert_file = cert_path;
	cfg.key_file = key_path;
	cfg.http3.enabled = true;

	Router router;
	router.get("/", [](HttpRequestView const &) {
		return HttpResponse::html("<h1>conflux HTTP/3</h1><p>Try /ping over h1, h2, or h3.</p>");
	});
	router.get("/ping", [](HttpRequestView const &) { return HttpResponse::json(R"({"transport":"h3-ready"})"); });

	HttpServer srv{cfg, std::move(router)};
	std::println(std::cerr, "HTTPS + HTTP/3 server listening on https://localhost:9443");
	srv.run();
}
