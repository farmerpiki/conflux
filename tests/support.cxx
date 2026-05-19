module;
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

export module conflux.tests.support;

import std;
import conflux.types;
import conflux.net.http;
export namespace conflux::tests {

std::string read_one_response(
	int fd) {
	std::string response;
	std::array<char, 4096> buf{};
	for (;;) {
		auto const n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n <= 0) {
			break;
		}
		response.append(buf.data(), static_cast<std::size_t>(n));

		auto const hdr_end = response.find("\r\n\r\n");
		if (hdr_end == std::string::npos) {
			continue;
		}

		auto cl_pos = response.find("Content-Length: ");
		if (cl_pos == std::string::npos || cl_pos > hdr_end) {
			break;
		}
		cl_pos += 16;
		auto const cl_end = response.find("\r\n", cl_pos);
		std::size_t body_len = 0;
	std::from_chars(response.data() + cl_pos, response.data() + cl_end, body_len);
		if (response.size() >= hdr_end + 4 + body_len) {
			break;
		}
	}
	return response;
}

class LocalTcpClient {
	int fd_ = -1;

public:
	explicit LocalTcpClient(
		std::uint16_t port)
		: fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
		if (fd_ < 0) {
			throw std::runtime_error{"socket failed"};
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

		if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
			close();
			throw std::runtime_error{"connect failed"};
		}
	}
	~LocalTcpClient() { close(); }
	LocalTcpClient(LocalTcpClient const &) = delete;
	LocalTcpClient &operator =(LocalTcpClient const &) = delete;
	LocalTcpClient(
		LocalTcpClient &&other) noexcept
		: fd_(std::exchange(other.fd_, -1)) {}
	LocalTcpClient &operator =(
		LocalTcpClient &&other) noexcept {
		if (this != &other) {
			close();
			fd_ = std::exchange(other.fd_, -1);
		}
		return *this;
	}
	[[nodiscard]] int fd() const noexcept { return fd_; }
	void close() noexcept {
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}
	[[nodiscard]] ssize_t send(
		std::string_view data,
		int flags = 0) const {
		return ::send(fd_, data.data(), data.size(), flags);
	}
	ssize_t recv(
		char *data,
		std::size_t size,
		int flags = 0) const {
		return ::recv(fd_, data, size, flags);
	}
	void set_recv_timeout(
		std::chrono::seconds timeout) const {
		timeval tv{.tv_sec = timeout.count(), .tv_usec = 0};
		::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	[[nodiscard]] std::string read_until_close() const {
		std::string response;
		std::array<char, 4096> buf{};
		for (;;) {
			auto const n = recv(buf.data(), buf.size());
			if (n <= 0) {
				break;
			}
			response.append(buf.data(), static_cast<std::size_t>(n));
		}
		return response;
	}
	[[nodiscard]] std::string read_one_response() const {
		return ::conflux::tests::read_one_response(fd_);
	}
	[[nodiscard]] std::string read_headers() const {
		std::string response;
		std::array<char, 4096> buf{};
		for (;;) {
			auto const n = recv(buf.data(), buf.size());
			if (n <= 0) {
				break;
			}
			response.append(buf.data(), static_cast<std::size_t>(n));
			if (response.find("\r\n\r\n") != std::string::npos) {
				break;
			}
		}
		return response;
	}
};
std::string http_request_on(
	std::uint16_t port,
	std::string_view method,
	std::string_view path,
	std::string_view content_type = "",
	std::string_view body = "",
	std::string_view extra_headers = "",
	std::string_view host = "localhost") {
	LocalTcpClient const client{port};

	std::string request;
	if (content_type.empty() && body.empty()) {
		request = std::format("{} {} HTTP/1.1\r\nHost: {}\r\n{}\r\n", method, path, host, extra_headers);
	} else {
		request = std::format(
			"{} {} HTTP/1.1\r\nHost: {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n{}\r\n{}",
			method,
			path,
			host,
			content_type,
			body.size(),
			extra_headers,
			body);
	}
	(void)client.send(request);
	return client.read_one_response();
}
std::string http_get_on(
	std::uint16_t port,
	std::string_view path,
	std::string_view extra_headers = "") {
	return http_request_on(port, "GET", path, "", "", extra_headers);
}
std::string http_get_on_host(
	std::uint16_t port,
	std::string_view host,
	std::string_view path,
	std::string_view extra_headers = "") {
	return http_request_on(port, "GET", path, "", "", extra_headers, host);
}
std::string http_post_on(
	std::uint16_t port,
	std::string_view path,
	std::string_view content_type,
	std::string_view body,
	std::string_view extra_headers = "") {
	return http_request_on(port, "POST", path, content_type, body, extra_headers);
}
std::string http_options_on(
	std::uint16_t port,
	std::string_view path,
	std::string_view extra_headers = "") {
	LocalTcpClient const client{port};
	auto request = std::format("OPTIONS {} HTTP/1.1\r\nHost: localhost\r\n{}\r\n", path, extra_headers);
	(void)client.send(request);
	client.set_recv_timeout(std::chrono::seconds{2});
	return client.read_headers();
}
void wait_for_server(
	std::uint16_t port) {
	constexpr int max_tries = 100;
	for (int i = 0; i < max_tries; ++i) {
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
class TestServerRegistry {
		std::mutex mu_;
	std::vector<std::shared_ptr<HttpServer>> servers_;
		std::vector<std::thread> threads_;

public:
	std::uint16_t start(
		Config const &cfg,
		Router router) {
		// TLS server probing in wait_for_server() triggers SIGPIPE without this.
		(void)::signal(SIGPIPE, SIG_IGN);
		auto srv = std::make_shared<HttpServer>(cfg, std::move(router));
		{
			std::lock_guard const lock{mu_};
			threads_.emplace_back([srv] { (void)srv->run(); });
			servers_.push_back(srv);
		}
		auto const port = srv->port();
		wait_for_server(port);
		return port;
	}
	std::uint16_t start(
		Config const &cfg,
		VHostRouter vhost_router) {
		(void)::signal(SIGPIPE, SIG_IGN);
		auto srv = std::make_shared<HttpServer>(cfg, std::move(vhost_router));
		{
			std::lock_guard const lock{mu_};
			threads_.emplace_back([srv] { (void)srv->run(); });
			servers_.push_back(srv);
		}
		auto const port = srv->port();
		wait_for_server(port);
		return port;
	}
	~TestServerRegistry() {
		for (auto const &srv: servers_) {
			srv->request_shutdown();
		}
		for (auto &thread: threads_) {
			if (thread.joinable()) {
				thread.join();
			}
		}
	}
};
TestServerRegistry &test_servers() {
	static TestServerRegistry registry;
	return registry;
}
class ScopedTestServer {
	std::shared_ptr<HttpServer> server_;
		std::thread thread_;

public:
	ScopedTestServer(
		Config const &cfg,
		Router router)
		: server_([&] {
			auto local_cfg = cfg;
			local_cfg.startup_banner = false;
			return std::make_shared<HttpServer>(local_cfg, std::move(router));
		}())
		, thread_([srv = server_] { (void)srv->run(); }) {
		wait_for_server(server_->port());
	}
	[[nodiscard]] std::uint16_t port() const { return server_->port(); }
	void stop() {
		if (thread_.joinable()) {
			server_->request_shutdown();
			thread_.join();
		}
	}
	~ScopedTestServer() { stop(); }
	ScopedTestServer(ScopedTestServer const &) = delete;
	ScopedTestServer &operator =(ScopedTestServer const &) = delete;
	ScopedTestServer(ScopedTestServer &&) = delete;
	ScopedTestServer &operator =(ScopedTestServer &&) = delete;
};
[[nodiscard]] Config mw_config() {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.startup_banner = false;
	// Opt out of per-ring file_io pools — middleware tests don't exercise file
	// I/O, and per-ring mlock accounting would accumulate across the many
	// servers registered in the static TestServerRegistry.
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	return cfg;
}
std::uint16_t start_mw_server(
	Config const &cfg,
	Router router) {
	return test_servers().start(cfg, std::move(router));
}

} // namespace conflux::tests
