#include <cstdlib>
#include <unistd.h>
import conflux.net.http;
import std;
import conflux.types;

int main() {
	S cert_path = "/tmp/conflux_h3_probe_cert.pem";
	S key_path = "/tmp/conflux_h3_probe_key.pem";
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
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.cert_file = cert_path;
	cfg.key_file = key_path;
	cfg.http3.enabled = true;

	Router router;
	router.get("/ping", [](HttpRequestView const &) { return HttpResponse::json(R"({"ok":true})"); });

	HttpServer srv{cfg, std::move(router)};
	std::println(std::cerr, "h3 probe running on :9443");
	srv.run();
}
