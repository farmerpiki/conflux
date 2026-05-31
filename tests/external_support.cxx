module;
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
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
namespace {

struct TempPemPair {
	std::string cert;
	std::string key;
	TempPemPair(
		std::string cert_pattern,
		std::string key_pattern) {
		auto open_temp = [](std::string &pattern) {
			std::vector<char> buf(pattern.begin(), pattern.end());
			buf.push_back('\0');
			int const fd = ::mkstemps(buf.data(), 4);
			if (fd < 0) {
				throw std::runtime_error{"mkstemps failed"};
			}
			::close(fd);
			pattern = buf.data();
		};
		cert = std::move(cert_pattern);
		key = std::move(key_pattern);
		try {
			open_temp(cert);
			open_temp(key);
		} catch (...) {
			if (!cert.empty()) {
				::unlink(cert.c_str());
			}
			throw;
		}
	}
	~TempPemPair() {
		if (!cert.empty()) {
			::unlink(cert.c_str());
		}
		if (!key.empty()) {
			::unlink(key.c_str());
		}
	}
	TempPemPair(TempPemPair const &) = delete;
	TempPemPair &operator =(TempPemPair const &) = delete;
	void release() noexcept {
		cert.clear();
		key.clear();
	}
};

} // namespace

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
// Generate a self-signed localhost cert once per process, returning (cert_pem,
// key_pem) bytes. Subsequent fixtures reuse the cached strings, avoiding the
// per-suite cost of spawning openssl.
std::pair<std::string, std::string> const &cached_test_cert() {
	static std::pair<std::string, std::string> const bytes = [] {
		TempPemPair tmp{"/tmp/conflux_cached_cert_XXXXXX.pem", "/tmp/conflux_cached_key_XXXXXX.pem"};
		std::string const cmd = std::format(
			"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
			"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
			tmp.key,
			tmp.cert);
		if (::system(cmd.c_str()) != 0) {
			throw std::runtime_error{"openssl req failed"};
		}
		auto slurp = [](char const *path) {
			std::ifstream in{path, std::ios::binary};
			std::stringstream ss;
			ss << in.rdbuf();
			return ss.str();
		};
		std::pair<std::string, std::string> out{slurp(tmp.cert.c_str()), slurp(tmp.key.c_str())};
		return out;
	}();
	return bytes;
}
// Materialize the cached cert bytes into a unique file P. HttpServer needs
// paths on disk; we unlink after the server starts.
std::pair<std::string, std::string> write_cached_cert_files() {
	auto const &[cert_pem, key_pem] = cached_test_cert();
	TempPemPair tmp{"/tmp/conflux_ext_cert_XXXXXX.pem", "/tmp/conflux_ext_key_XXXXXX.pem"};
	{
		int const f = ::open(tmp.cert.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
		if (f < 0) {
			throw std::runtime_error{"cert open failed"};
		}
		if (::write(f, cert_pem.data(), cert_pem.size()) != static_cast<ssize_t>(cert_pem.size())) {
			::close(f);
			throw std::runtime_error{"cert write failed"};
		}
		::close(f);
	}
	{
		int const f = ::open(tmp.key.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
		if (f < 0) {
			throw std::runtime_error{"key open failed"};
		}
		if (::write(f, key_pem.data(), key_pem.size()) != static_cast<ssize_t>(key_pem.size())) {
			::close(f);
			throw std::runtime_error{"key write failed"};
		}
		::close(f);
	}
	auto out = std::pair<std::string, std::string>{tmp.cert, tmp.key};
	tmp.release();
	return out;
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
	std::string cert_path_;
	std::string key_path_;
	std::shared_ptr<HttpServer> server_;
	std::thread srv_thread_;
	std::uint16_t port_{};
	void generate_cert() {
		auto [cert, key] = write_cached_cert_files();
		cert_path_ = std::move(cert);
		key_path_ = std::move(key);
	}

public:
	HttpsServerFixture(HttpsServerFixture const &) = delete;
	HttpsServerFixture &operator =(HttpsServerFixture const &) = delete;
	explicit HttpsServerFixture(
		conflux::http::Router router)
		: HttpsServerFixture(conflux::http::Config::test(), std::move(router)) {}
	HttpsServerFixture(
		conflux::http::Config cfg,
		conflux::http::Router router) {
		generate_cert();

		cfg.port = 0;
		cfg.startup_banner = false;
		cfg.cert_file = cert_path_;
		cfg.key_file = key_path_;
		apply_external_server_env(cfg);

		server_ = std::make_shared<HttpServer>(cfg, std::move(router));
		srv_thread_ = std::thread([srv = server_] { (void)srv->run(); });

		port_ = server_->port();
		wait_for_port(port_);
		wait_for_https(port_);

		::unlink(cert_path_.c_str());
		::unlink(key_path_.c_str());
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
	std::string cert_path_;
	std::string key_path_;
	std::shared_ptr<HttpServer> server_;
	std::thread srv_thread_;
	std::uint16_t port_{};
	void generate_cert() {
		auto [cert, key] = write_cached_cert_files();
		cert_path_ = std::move(cert);
		key_path_ = std::move(key);
	}

public:
	Http3ServerFixture(Http3ServerFixture const &) = delete;
	Http3ServerFixture &operator =(Http3ServerFixture const &) = delete;
	explicit Http3ServerFixture(
		conflux::http::Router router) {
		generate_cert();

		conflux::http::Config cfg{};
		cfg.port = 0;
		cfg.rings = 1;
		cfg.ring_entries = 256;
		cfg.single_issuer = true;
		cfg.defer_taskrun = true;
		cfg.coop_taskrun = true;
		cfg.taskrun_flag = true;
		cfg.startup_banner = false;
		cfg.cert_file = cert_path_;
		cfg.key_file = key_path_;
		cfg.http3.enabled = true;

		server_ = std::make_shared<HttpServer>(cfg, std::move(router));
		srv_thread_ = std::thread([srv = server_] { (void)srv->run(); });

		port_ = server_->port();
		wait_for_port(port_);
		wait_for_https(port_);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		::unlink(cert_path_.c_str());
		::unlink(key_path_.c_str());
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
