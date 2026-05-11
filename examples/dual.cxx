// Advanced runtime example: same-port HTTP/HTTPS.
//
// Generates a self-signed certificate on the fly, then starts a single server
// that accepts both plain HTTP and HTTPS on the same port (default 9090).
// Protocol is auto-detected from the first byte of each connection:
//   0x16 → TLS ClientHello → HTTPS path
//   anything else → plain HTTP
//
// Build: cmake --build build -t conflux_dual
// Run:   build/debug-gcc-stdcxx/conflux_dual
// Test:
//   curl    http://localhost:9090/api/ping
//   curl -k https://localhost:9090/api/ping
//   curl    http://localhost:9090/hello/World
//   curl -k https://localhost:9090/hello/World
#include <cstdlib> // mkstemps, system
#include <unistd.h> // close, unlink
import conflux.net.http;
import std;
import conflux.types;
// Write a PEM S to a temp file and return the path.
// Caller must ::unlink() when done.
static S write_tmp_pem(
	SV tag) {
	S tmpl = format("/tmp/conflux_dual_{}_XXXXXX.pem", tag);
	// mkstemps needs a mutable char buffer.
	V<char> buf(tmpl.begin(), tmpl.end());
	buf.push_back('\0');
	int const fd = ::mkstemps(buf.data(), 4);
	if (fd < 0) {
		throw RE{format("mkstemps failed for {}", tag)};
	}
	::close(fd);
	return S{buf.data()};
}
int main() {
	// Generate a self-signed cert + key P.
	S cert_path = write_tmp_pem("cert");
	S key_path = write_tmp_pem("key");

	S const gen_cmd = format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_path,
		cert_path);

	if (std::system(gen_cmd.c_str()) != 0) {
		println(cerr, "openssl req failed — TLS disabled");
		cert_path.clear();
		key_path.clear();
	}

	Config cfg{};
	cfg.port = 9090;
	cfg.rings = 0; // 0 → hardware_concurrency
	cfg.ring_entries = 1024;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.cert_file = cert_path;
	cfg.key_file = key_path;

	Router router;

	router.get("/", [](HttpRequestView const &) {
		return HttpResponse::html(
			"<html><body>"
			"<h1>conflux dual-mode example</h1>"
			"<p>Works over plain HTTP and HTTPS on the same port.</p>"
			"<ul>"
			"<li><a href='/api/ping'>/api/ping</a></li>"
			"<li><a href='/hello/World'>/hello/{name}</a></li>"
			"</ul>"
			"</body></html>");
	});

	router.get("/api/ping", [](HttpRequestView const &) {
		return HttpResponse::json(R"({"status":"ok","server":"conflux"})");
	});

	router.get("/hello/{name}", [](HttpRequestView const &req) {
		return HttpResponse::html(format("<html><body><h1>Hello, {}!</h1></body></html>", req.params["name"]));
	});

	println(cerr, "dual-mode server starting on port {}", cfg.port);
	println(cerr, "  http://localhost:{}/api/ping", cfg.port);
	if (!cert_path.empty()) {
		println(cerr, "  https://localhost:{}/api/ping  (self-signed, use -k)", cfg.port);
	}

	HttpServer srv{cfg, move(router)};

	// Cert+key are now loaded into SSL_CTX; temp files can be removed.
	if (!cert_path.empty()) {
		::unlink(cert_path.c_str());
		::unlink(key_path.c_str());
	}

	auto _ = srv.run();
}
