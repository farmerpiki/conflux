// Plain TU — not a module unit.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.proxy;
import conflux.net.router;
import conflux.net.proxy;
import conflux.tests.support;

using namespace conflux::tests;
namespace {

enum class HostileMode : std::uint8_t {
	close_mid_header,
	close_mid_body,
	malformed_chunked,
	oversized_headers,
	slow_response,
	redirect_loop,
};

class HostileUpstream {
public:
	explicit HostileUpstream(
		HostileMode mode)
		: mode_{mode} {
		listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
		if (listen_fd_ < 0) {
			throw std::runtime_error{"socket failed"};
		}
		int const one = 1;
		::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sa.sin_port = 0;
		if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0) {
			throw std::runtime_error{"bind failed"};
		}
		if (::listen(listen_fd_, 1) < 0) {
			throw std::runtime_error{"listen failed"};
		}
		sockaddr_in bound{};
		socklen_t len = sizeof(bound);
		if (::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&bound), &len) < 0) {
			throw std::runtime_error{"getsockname failed"};
		}
		port_ = ntohs(bound.sin_port);
		thread_ = std::jthread{[this](std::stop_token st) { run(st); }};
	}
	~HostileUpstream() {
		thread_.request_stop();
		int const cfd = client_fd_.load(std::memory_order_acquire);
		if (cfd >= 0) {
			::shutdown(cfd, SHUT_RDWR);
		}
		if (listen_fd_ >= 0) {
			::shutdown(listen_fd_, SHUT_RDWR);
			::close(listen_fd_);
		}
	}
	HostileUpstream(HostileUpstream const &) = delete;
	HostileUpstream &operator =(HostileUpstream const &) = delete;

	[[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
	void run(
		std::stop_token st) noexcept {
		::timeval tv{.tv_sec = 0, .tv_usec = 20000};
		::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		int fd = -1;
		while (!st.stop_requested()) {
			fd = ::accept(listen_fd_, nullptr, nullptr);
			if (fd >= 0) {
				break;
			}
		}
		if (fd < 0) {
			return;
		}
		client_fd_.store(fd, std::memory_order_release);
		drain_request(fd, st);
		write_script(fd, st);
		::close(fd);
		client_fd_.store(-1, std::memory_order_release);
	}

	static void drain_request(
		int fd,
		std::stop_token st) noexcept {
		::timeval tv{.tv_sec = 0, .tv_usec = 20000};
		::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		std::string req;
		std::array<char, 4096> buf{};
		while (!st.stop_requested() && req.find("\r\n\r\n") == std::string::npos) {
			auto const n = ::recv(fd, buf.data(), buf.size(), 0);
			if (n > 0) {
				req.append(buf.data(), static_cast<std::size_t>(n));
				continue;
			}
			if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
				return;
			}
		}
	}

	void write_script(
		int fd,
		std::stop_token st) const noexcept {
		switch (mode_) {
		case HostileMode::close_mid_header: write_all(fd, "HTTP/1.1 200 OK\r\nContent-Len"); return;
		case HostileMode::close_mid_body:
			write_all(fd, "HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\nab");
			return;
		case HostileMode::malformed_chunked:
			write_all(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\nZ\r\nbad\r\n");
			return;
		case HostileMode::oversized_headers:
			{
				std::string response = "HTTP/1.1 200 OK\r\nX-Big: ";
				response.append(72UL * 1024, 'x');
				response.append("\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
				write_all(fd, response);
				return;
			}
		case HostileMode::slow_response:
			while (!st.stop_requested()) {
				std::this_thread::sleep_for(std::chrono::milliseconds{20});
			}
			return;
		case HostileMode::redirect_loop:
			write_all(fd, "HTTP/1.1 302 Found\r\nLocation: /proxy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
			return;
		}
	}

	static void write_all(
		int fd,
		std::string_view wire) noexcept {
		std::size_t off = 0;
		while (off < wire.size()) {
			auto const n = ::send(fd, wire.data() + off, wire.size() - off, MSG_NOSIGNAL);
			if (n <= 0) {
				return;
			}
			off += static_cast<std::size_t>(n);
		}
	}

	HostileMode mode_{};
	int listen_fd_{-1};
	std::uint16_t port_{0};
	std::atomic<int> client_fd_{-1};
	std::jthread thread_{};
};

ScopedTestServer proxy_server_for(
	HostileUpstream const &upstream,
	int timeout_sec = 2) {
	Router front;
	auto popts = ProxyOptions{
		.upstream_host = "127.0.0.1",
		.upstream_port = upstream.port(),
		.timeout_sec = timeout_sec,
	};
	front.get("/proxy", [popts = std::move(popts)](RequestView const &req) { return blocking_proxy(req, popts); });
	return ScopedTestServer{mw_config(), std::move(front)};
}

std::string proxy_get_close(
	std::uint16_t front_port) {
	LocalTcpClient client{front_port};
	client.set_recv_timeout(std::chrono::seconds{4});
	std::string_view const req = "GET /proxy HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	REQUIRE(client.send(req, MSG_NOSIGNAL) == static_cast<ssize_t>(req.size()));
	return client.read_until_close();
}

void check_bad_gateway_for(
	HostileMode mode,
	int timeout_sec = 2) {
	HostileUpstream upstream{mode};
	auto front = proxy_server_for(upstream, timeout_sec);
	auto const response = proxy_get_close(front.port());
	REQUIRE(response.starts_with("HTTP/1.1 502 Bad Gateway"));
	CHECK(response.find("proxy:") != std::string::npos);
}

} // namespace

TEST_CASE(
	"proxy e2e: upstream close mid-header returns bad gateway",
	"[proxy][e2e][hostile]") {
	check_bad_gateway_for(HostileMode::close_mid_header);
}

TEST_CASE(
	"proxy e2e: upstream close mid-body returns bad gateway",
	"[proxy][e2e][hostile]") {
	check_bad_gateway_for(HostileMode::close_mid_body);
}

TEST_CASE(
	"proxy e2e: upstream malformed chunked body returns bad gateway",
	"[proxy][e2e][hostile]") {
	check_bad_gateway_for(HostileMode::malformed_chunked);
}

TEST_CASE(
	"proxy e2e: upstream oversized headers return bad gateway",
	"[proxy][e2e][hostile]") {
	check_bad_gateway_for(HostileMode::oversized_headers);
}

TEST_CASE(
	"proxy e2e: slow upstream response returns bad gateway",
	"[proxy][e2e][hostile][slow]") {
	check_bad_gateway_for(HostileMode::slow_response, 1);
}

TEST_CASE(
	"proxy e2e: upstream redirect loop is passed through, not followed",
	"[proxy][e2e][hostile]") {
	HostileUpstream upstream{HostileMode::redirect_loop};
	auto front = proxy_server_for(upstream);
	auto const response = proxy_get_close(front.port());
	REQUIRE(response.starts_with("HTTP/1.1 302 Found"));
	CHECK(response.find("Location: /proxy\r\n") != std::string::npos);
	CHECK(response.find("502 Bad Gateway") == std::string::npos);
}
