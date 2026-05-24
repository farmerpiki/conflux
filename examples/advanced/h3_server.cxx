// Advanced runtime example: explicit HTTPS + HTTP/3 server config.
//
// Run:
//   build/release-clang-libcxx/conflux_h3_server
//
// Try:
//   curl -k https://localhost:9443/ping
//   curl -k --http3-only https://localhost:9443/ping
#include <cstdlib>
#include <unistd.h>

import conflux.net.config;
import conflux.net.http_server;
import conflux.net.router;
import std;
import conflux.types;
namespace {

struct TempCertFiles {
	std::string cert;
	std::string key;
	~TempCertFiles() {
		if (!cert.empty()) {
			::unlink(cert.c_str());
		}
		if (!key.empty()) {
			::unlink(key.c_str());
		}
	}
};

std::string make_temp_pem(
	std::string_view tag) {
	std::string tmpl = std::format("/tmp/conflux_h3_server_{}_XXXXXX.pem", tag);
	std::vector<char> buf(tmpl.begin(), tmpl.end());
	buf.push_back('\0');
	int const fd = ::mkstemps(buf.data(), 4);
	if (fd < 0) {
		throw std::runtime_error{"mkstemps failed"};
	}
	::close(fd);
	return buf.data();
}

} // namespace

int main() {
	TempCertFiles cert_files;
	try {
		cert_files.cert = make_temp_pem("cert");
		cert_files.key = make_temp_pem("key");
	} catch (std::exception const &e) {
		std::println(std::cerr, "temporary cert path failed: {}", e.what());
		return 1;
	}
	std::string const gen_cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		cert_files.key,
		cert_files.cert);
	if (std::system(gen_cmd.c_str()) != 0) {
		std::println(std::cerr, "openssl req failed");
		return 1;
	}

	Config cfg{};
	cfg.port = 9443;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.cert_file = cert_files.cert;
	cfg.key_file = cert_files.key;
	cfg.http3.enabled = true;

	Router router;
	router.get("/", [](RequestView const &) {
		return Response::html("<h1>conflux HTTP/3</h1><p>Try /ping over h1, h2, or h3.</p>");
	});
	router.get("/ping", [](RequestView const &) { return Response::json(R"({"transport":"h3-ready"})"); });

	HttpServer srv{cfg, std::move(router)};
	::unlink(cert_files.cert.c_str());
	::unlink(cert_files.key.c_str());
	cert_files.cert.clear();
	cert_files.key.clear();
	std::println(std::cerr, "HTTPS + HTTP/3 server listening on https://localhost:9443");
	auto const status = srv.run();
	return status == RunStatus::stopped_normally ? 0 : 1;
}
