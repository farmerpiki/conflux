module;
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

export module conflux.tests.support;

import std;
import conflux.net.http;

using namespace std;

export namespace conflux::tests {

class LocalTcpClient {
	int fd_ = -1;

public:
	explicit LocalTcpClient(
		uint16_t port)
		: fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
		if (fd_ < 0) {
			throw runtime_error{"socket failed"};
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

		if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
			close();
			throw runtime_error{"connect failed"};
		}
	}

	~LocalTcpClient() { close(); }

	LocalTcpClient(LocalTcpClient const &) = delete;
	LocalTcpClient &operator =(LocalTcpClient const &) = delete;

	LocalTcpClient(
		LocalTcpClient &&other) noexcept
		: fd_(exchange(other.fd_, -1)) {}

	LocalTcpClient &operator =(
		LocalTcpClient &&other) noexcept {
		if (this != &other) {
			close();
			fd_ = exchange(other.fd_, -1);
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
		string_view data,
		int flags = 0) const {
		return ::send(fd_, data.data(), data.size(), flags);
	}

	ssize_t recv(
		char *data,
		size_t size,
		int flags = 0) const {
		return ::recv(fd_, data, size, flags);
	}

	void set_recv_timeout(
		chrono::seconds timeout) const {
		timeval tv{.tv_sec = timeout.count(), .tv_usec = 0};
		::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	[[nodiscard]] string read_until_close() const {
		string response;
		array<char, 4096> buf{};
		for (;;) {
			auto const n = recv(buf.data(), buf.size());
			if (n <= 0) {
				break;
			}
			response.append(buf.data(), static_cast<size_t>(n));
		}
		return response;
	}

	[[nodiscard]] string read_one_response() const {
		string response;
		array<char, 4096> buf{};
		for (;;) {
			auto const n = recv(buf.data(), buf.size());
			if (n <= 0) {
				break;
			}
			response.append(buf.data(), static_cast<size_t>(n));

			auto const hdr_end = response.find("\r\n\r\n");
			if (hdr_end == string::npos) {
				continue;
			}

			auto cl_pos = response.find("Content-Length: ");
			if (cl_pos == string::npos || cl_pos > hdr_end) {
				break;
			}
			cl_pos += 16;
			auto const cl_end = response.find("\r\n", cl_pos);
			size_t body_len = 0;
			from_chars(response.data() + cl_pos, response.data() + cl_end, body_len);
			if (response.size() >= hdr_end + 4 + body_len) {
				break;
			}
		}
		return response;
	}

	[[nodiscard]] string read_headers() const {
		string response;
		array<char, 4096> buf{};
		for (;;) {
			auto const n = recv(buf.data(), buf.size());
			if (n <= 0) {
				break;
			}
			response.append(buf.data(), static_cast<size_t>(n));
			if (response.find("\r\n\r\n") != string::npos) {
				break;
			}
		}
		return response;
	}
};

string read_one_response(
	int fd) {
	string response;
	array<char, 4096> buf{};
	for (;;) {
		auto const n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n <= 0) {
			break;
		}
		response.append(buf.data(), static_cast<size_t>(n));

		auto const hdr_end = response.find("\r\n\r\n");
		if (hdr_end == string::npos) {
			continue;
		}

		auto cl_pos = response.find("Content-Length: ");
		if (cl_pos == string::npos || cl_pos > hdr_end) {
			break;
		}
		cl_pos += 16;
		auto const cl_end = response.find("\r\n", cl_pos);
		size_t body_len = 0;
		from_chars(response.data() + cl_pos, response.data() + cl_end, body_len);
		if (response.size() >= hdr_end + 4 + body_len) {
			break;
		}
	}
	return response;
}

string http_request_on(
	uint16_t port,
	string_view method,
	string_view path,
	string_view content_type = "",
	string_view body = "",
	string_view extra_headers = "",
	string_view host = "localhost") {
	LocalTcpClient const client{port};

	string request;
	if (content_type.empty() && body.empty()) {
		request = format("{} {} HTTP/1.1\r\nHost: {}\r\n{}\r\n", method, path, host, extra_headers);
	} else {
		request = format(
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

string http_get_on(
	uint16_t port,
	string_view path,
	string_view extra_headers = "") {
	return http_request_on(port, "GET", path, "", "", extra_headers);
}

string http_get_on_host(
	uint16_t port,
	string_view host,
	string_view path,
	string_view extra_headers = "") {
	return http_request_on(port, "GET", path, "", "", extra_headers, host);
}

string http_post_on(
	uint16_t port,
	string_view path,
	string_view content_type,
	string_view body,
	string_view extra_headers = "") {
	return http_request_on(port, "POST", path, content_type, body, extra_headers);
}

string http_options_on(
	uint16_t port,
	string_view path,
	string_view extra_headers = "") {
	LocalTcpClient const client{port};
	auto request = format("OPTIONS {} HTTP/1.1\r\nHost: localhost\r\n{}\r\n", path, extra_headers);
	(void)client.send(request);
	client.set_recv_timeout(chrono::seconds{2});
	return client.read_headers();
}

void wait_for_server(
	uint16_t port) {
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
		this_thread::sleep_for(chrono::milliseconds(10));
	}
	throw runtime_error{"server did not start in time"};
}

class TestServerRegistry {
	mutex mu_;
	vector<shared_ptr<HttpServer>> servers_;
	vector<thread> threads_;

public:
	uint16_t start(
		Config const &cfg,
		Router router) {
		// TLS server probing in wait_for_server() triggers SIGPIPE without this.
		(void)::signal(SIGPIPE, SIG_IGN);
		auto srv = make_shared<HttpServer>(cfg, move(router));
		{
			lock_guard const lock{mu_};
			threads_.emplace_back([srv] {
				try {
					srv->run();
				} catch (exception const &ex) {
					println(cerr, "HttpServer test thread failed: {}", ex.what());
				} catch (...) { println(cerr, "HttpServer test thread failed: unknown exception"); }
			});
			servers_.push_back(srv);
		}
		auto const port = srv->port();
		wait_for_server(port);
		this_thread::sleep_for(chrono::milliseconds(20));
		return port;
	}

	uint16_t start(
		Config const &cfg,
		VHostRouter vhost_router) {
		(void)::signal(SIGPIPE, SIG_IGN);
		auto srv = make_shared<HttpServer>(cfg, move(vhost_router));
		{
			lock_guard const lock{mu_};
			threads_.emplace_back([srv] {
				try {
					srv->run();
				} catch (exception const &ex) {
					println(cerr, "HttpServer test thread failed: {}", ex.what());
				} catch (...) { println(cerr, "HttpServer test thread failed: unknown exception"); }
			});
			servers_.push_back(srv);
		}
		auto const port = srv->port();
		wait_for_server(port);
		this_thread::sleep_for(chrono::milliseconds(20));
		return port;
	}

	~TestServerRegistry() {
		for (auto const &srv: servers_) {
			srv->shutdown();
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
	shared_ptr<HttpServer> server_;
	thread thread_;

public:
	ScopedTestServer(
		Config const &cfg,
		Router router)
		: server_([&] {
			auto local_cfg = cfg;
			local_cfg.startup_banner = false;
			return make_shared<HttpServer>(local_cfg, move(router));
		}())
		, thread_([srv = server_] {
			try {
				srv->run();
			} catch (exception const &ex) {
				println(cerr, "HttpServer test thread failed: {}", ex.what());
			} catch (...) { println(cerr, "HttpServer test thread failed: unknown exception"); }
		}) {
		wait_for_server(server_->port());
		// Give the io_uring ring one scheduler tick to drain the probe-close SQE
		// before returning — prevents fd reuse racing with the immediately-following
		// test connection on the same fd slot.
		this_thread::sleep_for(chrono::milliseconds(20));
	}

	[[nodiscard]] uint16_t port() const { return server_->port(); }

	void stop() {
		if (thread_.joinable()) {
			server_->shutdown();
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

uint16_t start_mw_server(
	Config const &cfg,
	Router router) {
	return test_servers().start(cfg, move(router));
}

} // namespace conflux::tests
