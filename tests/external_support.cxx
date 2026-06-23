module;
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

export module conflux.tests.external_support;
import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http_server;
import conflux.net.router;
import conflux.tests.support;

export namespace conflux::tests {

[[nodiscard]] std::pair<int, std::string> run_cmd(
	std::string const &cmd) {
	FILE *fp = ::popen(cmd.c_str(), "r");
	if (fp == nullptr) {
		return {-1, {}};
	}
	std::string out;
	std::array<char, 4096> buf{};
	while (::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
		out += buf.data();
	}
	int const status = ::pclose(fp);
	int const code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return {code, out};
}
[[nodiscard]] std::pair<int, std::string> run_cmd_retry(
	std::string const &cmd,
	int attempts = 3) {
	std::pair<int, std::string> result{-1, {}};
	for (int i = 0; i < attempts; ++i) {
		result = run_cmd(cmd);
		if (result.first == 0) {
			return result;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	return result;
}
[[nodiscard]] bool env_bool(
	char const *name,
	bool fallback) {
	char const *raw = std::getenv(name);
	if (raw == nullptr || *raw == '\0') {
		return fallback;
	}
	std::string_view const value{raw};
	if (value == "0" || value == "false" || value == "off" || value == "no") {
		return false;
	}
	return value == "1" || value == "true" || value == "on" || value == "yes";
}
void apply_external_server_env(
	conflux::http::Config &cfg) {
	cfg.recv_bundle = env_bool("CONFLUX_TEST_RECV_BUNDLE", cfg.recv_bundle);
	cfg.direct_accept = env_bool("CONFLUX_TEST_DIRECT_ACCEPT", cfg.direct_accept);
	cfg.cmd_sock_setsockopt = env_bool("CONFLUX_TEST_CMD_SOCK_SOCKOPTS", cfg.cmd_sock_setsockopt);
}
void wait_for_port(
	std::uint16_t port) {
	for (int i = 0; i < 100; ++i) {
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		bool const up = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
		::close(fd);
		if (up) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	throw std::runtime_error{"server did not start in time"};
}
void wait_for_https(
	std::uint16_t port) {
	for (int i = 0; i < 50; ++i) {
		auto [code, out] =
			run_cmd(std::format("curl -sk --http1.1 -o /dev/null --max-time 1 https://127.0.0.1:{}/", port));
		(void)out;
		if (code == 0) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	throw std::runtime_error{"https server did not become ready in time"};
}
class HttpsServerFixture {
	std::shared_ptr<conflux::http::HttpServer> server_;
	std::thread srv_thread_;
	std::uint16_t port_{};

public:
	HttpsServerFixture(HttpsServerFixture const &) = delete;
	HttpsServerFixture &operator =(HttpsServerFixture const &) = delete;
	explicit HttpsServerFixture(
		conflux::http::Router router)
		: HttpsServerFixture(conflux::http::Config::test(), std::move(router)) {}
	HttpsServerFixture(
		conflux::http::Config cfg,
		conflux::http::Router router) {
		cfg.port = 0;
		cfg.startup_banner = false;
		configure_test_tls(cfg);
		apply_external_server_env(cfg);

		server_ = std::make_shared<conflux::http::HttpServer>(cfg, std::move(router));
		srv_thread_ = std::thread([srv = server_] { (void)srv->run(); });

		port_ = server_->port();
		wait_for_port(port_);
		wait_for_https(port_);
	}
	~HttpsServerFixture() {
		if (server_ != nullptr) {
			server_->request_shutdown();
		}
		if (srv_thread_.joinable()) {
			srv_thread_.join();
		}
	}
	[[gnu::pure]] [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
	[[nodiscard]] std::pair<int, std::string> curl_https(
		std::string_view path) const {
		return run_cmd_retry(std::format("curl -sk --http1.1 --max-time 5 https://127.0.0.1:{}{}", port_, path));
	}
	[[nodiscard]] std::pair<int, std::string> curl_http(
		std::string_view path) const {
		return run_cmd_retry(std::format("curl -s --max-time 5 http://127.0.0.1:{}{}", port_, path));
	}
	[[nodiscard]] std::pair<int, std::string> curl_https_status(
		std::string_view path) const {
		return run_cmd_retry(
			std::format(
				"curl -sk --http1.1 -o /dev/null -w '%{{http_code}}' --max-time 5 "
				"https://127.0.0.1:{}{}",
				port_,
				path));
	}
	[[nodiscard]] std::string sclient_get(
		std::string_view path) const {
		auto const cmd = std::format(
			"printf 'GET {} HTTP/1.0\\r\\nHost: localhost\\r\\n\\r\\n' | "
			"openssl s_client -connect 127.0.0.1:{} -quiet -ign_eof 2>/dev/null",
			path,
			port_);
		std::string out;
		for (int i = 0; i < 3; ++i) {
			auto [code, attempt] = run_cmd(cmd);
			(void)code;
			out = std::move(attempt);
			if (!out.empty()) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		return out;
	}
};
class Http3ServerFixture {
	std::shared_ptr<conflux::http::HttpServer> server_;
	std::thread srv_thread_;
	std::uint16_t port_{};

public:
	Http3ServerFixture(Http3ServerFixture const &) = delete;
	Http3ServerFixture &operator =(Http3ServerFixture const &) = delete;
	explicit Http3ServerFixture(
		conflux::http::Router router)
		: Http3ServerFixture(conflux::http::Config{}, std::move(router)) {}
	Http3ServerFixture(
		conflux::http::Config cfg,
		conflux::http::Router router) {
		cfg.port = 0;
		cfg.rings = 1;
		cfg.ring_entries = 256;
		cfg.single_issuer = true;
		cfg.defer_taskrun = true;
		cfg.coop_taskrun = true;
		cfg.taskrun_flag = true;
		cfg.startup_banner = false;
		configure_test_tls(cfg);
		cfg.http3.enabled = true;

		server_ = std::make_shared<conflux::http::HttpServer>(cfg, std::move(router));
		srv_thread_ = std::thread([srv = server_] { (void)srv->run(); });

		port_ = server_->port();
		wait_for_port(port_);
		wait_for_https(port_);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	~Http3ServerFixture() {
		if (server_ != nullptr) {
			server_->request_shutdown();
		}
		if (srv_thread_.joinable()) {
			srv_thread_.join();
		}
	}
	[[gnu::pure]] [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
	[[nodiscard]] std::pair<int, std::string> curl_h3(
		std::string_view path) const {
		return run_cmd_retry(
			std::format(
				"curl -sk --http3-only --max-time 5 "
				"--resolve localhost:{}:127.0.0.1 https://localhost:{}{}",
				port_,
				port_,
				path));
	}
	[[nodiscard]] std::pair<int, std::string> curl_h3_status(
		std::string_view path) const {
		return run_cmd_retry(
			std::format(
				"curl -sk --http3-only -o /dev/null -w '%{{http_code}}' --max-time 5 "
				"--resolve localhost:{}:127.0.0.1 https://localhost:{}{}",
				port_,
				port_,
				path));
	}
};
[[nodiscard]] conflux::http::Router make_external_test_router() {
	conflux::http::Router r;
	r.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"ok":true})"); });
	r.get("/hello/{name}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::format("hello {}", req.params["name"]));
	});
	r.post("/echo", [](conflux::http::OwnedRequest const &req) { return conflux::http::Response::text(req.body); });
	return r;
}

} // namespace conflux::tests
