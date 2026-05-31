// Advanced runtime example: same-port HTTP/HTTPS.
//
// Generates a self-signed certificate on the fly, then starts a single server
// that accepts both plain HTTP and HTTPS on the same port (default 9090).
// Protocol is auto-detected from the first byte of each connection:
//   0x16 → TLS ClientHello → HTTPS path
//   anything else → plain HTTP
//
// Build: cmake --build build -t conflux_dual
// Run:   build/release-clang-libcxx/conflux_dual
// Test:
//   curl    http://localhost:9090/api/ping
//   curl -k https://localhost:9090/api/ping
//   curl    http://localhost:9090/hello/World
//   curl -k https://localhost:9090/hello/World
#include <cstdlib> // mkstemps, system
#include <unistd.h> // close, unlink
import conflux.net.config;
import conflux.net.http_server;
import conflux.net.router;
import std;
import conflux.types;
// Write a PEM std::string to a temp file and return the path.
// Caller must ::unlink() when done.
static std::string write_tmp_pem(
	std::string_view tag) {
	std::string tmpl = std::format("/tmp/conflux_dual_{}_XXXXXX.pem", tag);
	// mkstemps needs a mutable char buffer.
	std::vector<char> buf(tmpl.begin(), tmpl.end());
	buf.push_back('\0');
	int const fd = ::mkstemps(buf.data(), 4);
	if (fd < 0) {
		throw std::runtime_error{format("mkstemps failed for {}", tag)};
	}
	::close(fd);
	return std::string{buf.data()};
}
int main() {
	// Generate a self-signed cert + key P.
	std::string cert_path = write_tmp_pem("cert");
	std::string key_path = write_tmp_pem("key");

	std::string const gen_cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_path,
		cert_path);

	if (std::system(gen_cmd.c_str()) != 0) {
		std::println(std::cerr, "openssl req failed — TLS disabled");
		cert_path.clear();
		key_path.clear();
	}

	conflux::http::Config cfg{};
	cfg.port = 9090;
	cfg.rings = 0; // 0 → hardware_concurrency
	cfg.ring_entries = 1024;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.cert_file = cert_path;
	cfg.key_file = key_path;

	conflux::http::Router router;

	router.get("/", [](RequestView const &) {
		return conflux::http::Response::html(
			"<html><body>"
			"<h1>conflux dual-mode example</h1>"
			"<p>Works over plain HTTP and HTTPS on the same port.</p>"
			"<ul>"
			"<li><a href='/api/ping'>/api/ping</a></li>"
			"<li><a href='/hello/World'>/hello/{name}</a></li>"
			"</ul>"
			"</body></html>");
	});

	router.get("/api/ping", [](RequestView const &) {
		return conflux::http::Response::json(R"({"status":"ok","server":"conflux"})");
	});

	router.get("/hello/{name}", [](RequestView const &req) {
		return conflux::http::Response::html(
			std::format("<html><body><h1>Hello, {}!</h1></body></html>", req.params["name"]));
	});

	std::println(std::cerr, "dual-mode server starting on port {}", cfg.port);
	std::println(std::cerr, "  http://localhost:{}/api/ping", cfg.port);
	if (!cert_path.empty()) {
		std::println(std::cerr, "  https://localhost:{}/api/ping  (self-signed, use -k)", cfg.port);
	}

	HttpServer srv{cfg, std::move(router)};

	// Cert+key are now loaded into SSL_CTX; temp files can be removed.
	if (!cert_path.empty()) {
		::unlink(cert_path.c_str());
		::unlink(key_path.c_str());
	}

	auto const status = srv.run();
	return status == conflux::http::RunStatus::stopped_normally ? 0 : 1;
}
