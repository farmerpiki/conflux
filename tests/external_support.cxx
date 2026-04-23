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
import conflux.net.http;

using namespace std;

export namespace conflux::tests {

[[nodiscard]] pair<int, string> run_cmd(
	string const &cmd) {
	FILE *fp = ::popen(cmd.c_str(), "r");
	if (fp == nullptr) {
		return {-1, {}};
	}
	string out;
	array<char, 4096> buf{};
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
pair<string, string> const &cached_test_cert() {
	static pair<string, string> const bytes = [] {
		char cert_tmp[] = "/tmp/conflux_cached_cert_XXXXXX.pem";
		char key_tmp[] = "/tmp/conflux_cached_key_XXXXXX.pem";
		{
			int const f = ::mkstemps(cert_tmp, 4);
			::close(f);
		}
		{
			int const f = ::mkstemps(key_tmp, 4);
			::close(f);
		}
		string const cmd = format(
			"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
			"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
			key_tmp,
			cert_tmp);
		if (::system(cmd.c_str()) != 0) {
			throw runtime_error{"openssl req failed"};
		}
		auto slurp = [](char const *path) {
			ifstream in{path, ios::binary};
			stringstream ss;
			ss << in.rdbuf();
			return ss.str();
		};
		pair<string, string> out{slurp(cert_tmp), slurp(key_tmp)};
		::unlink(cert_tmp);
		::unlink(key_tmp);
		return out;
	}();
	return bytes;
}

// Materialize the cached cert bytes into a unique file pair. HttpServer needs
// paths on disk; we unlink after the server starts.
pair<string, string> write_cached_cert_files() {
	auto const &[cert_pem, key_pem] = cached_test_cert();
	char cert_tmp[] = "/tmp/conflux_ext_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_ext_key_XXXXXX.pem";
	{
		int const f = ::mkstemps(cert_tmp, 4);
		if (f < 0) {
			throw runtime_error{"mkstemps failed"};
		}
		::write(f, cert_pem.data(), cert_pem.size());
		::close(f);
	}
	{
		int const f = ::mkstemps(key_tmp, 4);
		if (f < 0) {
			throw runtime_error{"mkstemps failed"};
		}
		::write(f, key_pem.data(), key_pem.size());
		::close(f);
	}
	return {cert_tmp, key_tmp};
}

[[nodiscard]] pair<int, string> run_cmd_retry(
	string const &cmd,
	int attempts = 3) {
	pair<int, string> result{-1, {}};
	for (int i = 0; i < attempts; ++i) {
		result = run_cmd(cmd);
		if (result.first == 0) {
			return result;
		}
		this_thread::sleep_for(chrono::milliseconds(25));
	}
	return result;
}

void wait_for_port(
	uint16_t port) {
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
		this_thread::sleep_for(chrono::milliseconds(10));
	}
	throw runtime_error{"server did not start in time"};
}

void wait_for_https(
	uint16_t port) {
	for (int i = 0; i < 50; ++i) {
		auto [code, out] = run_cmd(format("curl -sk -o /dev/null --max-time 1 https://127.0.0.1:{}/", port));
		(void)out;
		if (code == 0) {
			return;
		}
		this_thread::sleep_for(chrono::milliseconds(20));
	}
	throw runtime_error{"https server did not become ready in time"};
}

class HttpsServerFixture {
	string cert_path_;
	string key_path_;
	shared_ptr<HttpServer> server_;
	thread srv_thread_;
	uint16_t port_{};

	void generate_cert() {
		auto [cert, key] = write_cached_cert_files();
		cert_path_ = move(cert);
		key_path_ = move(key);
	}

public:
	HttpsServerFixture(HttpsServerFixture const &) = delete;
	HttpsServerFixture &operator =(HttpsServerFixture const &) = delete;

	explicit HttpsServerFixture(
		Router router) {
		generate_cert();

		Config cfg{};
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

		server_ = make_shared<HttpServer>(cfg, move(router));
		srv_thread_ = thread([srv = server_] { srv->run(); });

		port_ = server_->port();
		wait_for_port(port_);
		wait_for_https(port_);

		::unlink(cert_path_.c_str());
		::unlink(key_path_.c_str());
	}

	~HttpsServerFixture() {
		if (server_ != nullptr) {
			server_->shutdown();
		}
		if (srv_thread_.joinable()) {
			srv_thread_.join();
		}
	}

	[[gnu::pure]] [[nodiscard]] uint16_t port() const noexcept { return port_; }

	[[nodiscard]] pair<int, string> curl_https(
		string_view path) const {
		return run_cmd_retry(format("curl -sk --max-time 5 https://127.0.0.1:{}{}", port_, path));
	}

	[[nodiscard]] pair<int, string> curl_http(
		string_view path) const {
		return run_cmd_retry(format("curl -s --max-time 5 http://127.0.0.1:{}{}", port_, path));
	}

	[[nodiscard]] pair<int, string> curl_https_status(
		string_view path) const {
		return run_cmd_retry(format(
			"curl -sk -o /dev/null -w '%{{http_code}}' --max-time 5 "
			"https://127.0.0.1:{}{}",
			port_,
			path));
	}

	[[nodiscard]] string sclient_get(
		string_view path) const {
		auto const cmd = format(
			"printf 'GET {} HTTP/1.0\\r\\nHost: localhost\\r\\n\\r\\n' | "
			"openssl s_client -connect 127.0.0.1:{} -quiet -ign_eof 2>/dev/null",
			path,
			port_);
		string out;
		for (int i = 0; i < 3; ++i) {
			auto [code, attempt] = run_cmd(cmd);
			(void)code;
			out = move(attempt);
			if (!out.empty()) {
				break;
			}
			this_thread::sleep_for(chrono::milliseconds(25));
		}
		return out;
	}
};

class Http3ServerFixture {
	string cert_path_;
	string key_path_;
	shared_ptr<HttpServer> server_;
	thread srv_thread_;
	uint16_t port_{};

	void generate_cert() {
		auto [cert, key] = write_cached_cert_files();
		cert_path_ = move(cert);
		key_path_ = move(key);
	}

public:
	Http3ServerFixture(Http3ServerFixture const &) = delete;
	Http3ServerFixture &operator =(Http3ServerFixture const &) = delete;

	explicit Http3ServerFixture(
		Router router) {
		generate_cert();

		Config cfg{};
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

		server_ = make_shared<HttpServer>(cfg, move(router));
		srv_thread_ = thread([srv = server_] { srv->run(); });

		port_ = server_->port();
		wait_for_port(port_);
		wait_for_https(port_);
		this_thread::sleep_for(chrono::milliseconds(50));

		::unlink(cert_path_.c_str());
		::unlink(key_path_.c_str());
	}

	~Http3ServerFixture() {
		if (server_ != nullptr) {
			server_->shutdown();
		}
		if (srv_thread_.joinable()) {
			srv_thread_.join();
		}
	}

	[[gnu::pure]] [[nodiscard]] uint16_t port() const noexcept { return port_; }

	[[nodiscard]] pair<int, string> curl_h3(
		string_view path) const {
		return run_cmd_retry(format(
			"curl -sk --http3-only --max-time 5 "
			"--resolve localhost:{}:127.0.0.1 https://localhost:{}{}",
			port_,
			port_,
			path));
	}

	[[nodiscard]] pair<int, string> curl_h3_status(
		string_view path) const {
		return run_cmd_retry(format(
			"curl -sk --http3-only -o /dev/null -w '%{{http_code}}' --max-time 5 "
			"--resolve localhost:{}:127.0.0.1 https://localhost:{}{}",
			port_,
			port_,
			path));
	}
};

[[nodiscard]] Router make_external_test_router() {
	Router r;
	r.get("/ping", [](HttpRequest const &) { return HttpResponse::json(R"({"ok":true})"); });
	r.get("/hello/{name}", [](HttpRequest const &req) {
		return HttpResponse::text(format("hello {}", req.params["name"]));
	});
	r.post("/echo", [](HttpRequest const &req) { return HttpResponse::text(req.body); });
	return r;
}

} // namespace conflux::tests
