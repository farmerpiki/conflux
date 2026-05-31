// Advanced runtime example: HTTP/3 probe with explicit ring/taskrun config.
#include <cstdlib>
#include <unistd.h>
import conflux.net.config;
import conflux.net.http_server;
import conflux.net.router;
import std;
import conflux.types;
int main() {
	auto make_tmp_pem = [](std::string_view tag) {
		std::string tmpl = std::format("/tmp/conflux_h3_probe_{}_XXXXXX.pem", tag);
		std::vector<char> buf(tmpl.begin(), tmpl.end());
		buf.push_back('\0');
		int const fd = ::mkstemps(buf.data(), 4);
		if (fd < 0) {
			throw std::runtime_error{std::format("mkstemps failed for {}", tag)};
		}
		::close(fd);
		return std::string{buf.data()};
	};
	std::string cert_path = make_tmp_pem("cert");
	std::string key_path = make_tmp_pem("key");
	std::string const gen_cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_path,
		cert_path);
	if (std::system(gen_cmd.c_str()) != 0) {
		std::println(std::cerr, "openssl req failed");
		::unlink(cert_path.c_str());
		::unlink(key_path.c_str());
		return 1;
	}

	conflux::http::Config cfg{};
	cfg.port = 9443;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.cert_file = cert_path;
	cfg.key_file = key_path;
	cfg.http3.enabled = true;

	conflux::http::Router router;
	router.get("/ping", [](conflux::http::RequestView const &) { return conflux::http::Response::json(R"({"ok":true})"); });

	HttpServer srv{cfg, std::move(router)};
	::unlink(cert_path.c_str());
	::unlink(key_path.c_str());
	std::println(std::cerr, "h3 probe running on :9443");
	auto const status = srv.run();
	return status == conflux::http::RunStatus::stopped_normally ? 0 : 1;
}
