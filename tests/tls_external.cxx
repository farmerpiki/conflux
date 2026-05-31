// External TLS validation tests.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <conflux/detail/discard.hxx>
#include <cstdlib>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http.realtime;
import conflux.net.http.static_files;
import conflux.net.http_server;
import conflux.net.router;
import conflux.tests.external_support;

namespace {

class TcpFd {
	int fd_{-1};

public:
	TcpFd() = default;
	explicit TcpFd(
		int fd) noexcept
		: fd_{fd} {}
	~TcpFd() {
		if (fd_ >= 0) {
			::close(fd_);
		}
	}
	TcpFd(TcpFd const &) = delete;
	TcpFd &operator =(TcpFd const &) = delete;
	TcpFd(
		TcpFd &&other) noexcept
		: fd_{std::exchange(other.fd_, -1)} {}
	TcpFd &operator =(
		TcpFd &&other) noexcept {
		if (this != &other) {
			if (fd_ >= 0) {
				::close(fd_);
			}
			fd_ = std::exchange(other.fd_, -1);
		}
		return *this;
	}
	[[nodiscard]] int get() const noexcept { return fd_; }
	void reset() noexcept {
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}
};

[[nodiscard]] TcpFd connect_loopback(
	std::uint16_t port) {
	TcpFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
	REQUIRE(fd.get() >= 0);

	::sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::connect(fd.get(), reinterpret_cast<::sockaddr *>(&addr), sizeof(addr)) == 0);
	return fd;
}

[[nodiscard]] std::string read_available(
	int fd,
	std::chrono::milliseconds budget) {
	std::string out;
	auto const deadline = std::chrono::steady_clock::now() + budget;
	std::array<char, 256> buf{};
	while (std::chrono::steady_clock::now() < deadline) {
		auto const remaining =
			std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		pollfd pfd{.fd = fd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0};
		int const rc = ::poll(&pfd, 1, static_cast<int>(std::max(remaining.count(), decltype(remaining.count()){1})));
		if (rc <= 0) {
			break;
		}
		if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
			continue;
		}
		ssize_t const n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n <= 0) {
			break;
		}
		out.append(buf.data(), static_cast<std::size_t>(n));
	}
	return out;
}

void require_https_ping_ok(
	conflux::tests::HttpsServerFixture const &fx) {
	auto [code, body] = fx.curl_https("/ping");
	REQUIRE(code == 0);
	REQUIRE(body == R"({"ok":true})");
}

std::optional<std::string> first_http_status_code(
	std::string_view output) {
	auto const line_begin = output.find("HTTP/1.");
	if (line_begin == std::string_view::npos) {
		return std::nullopt;
	}
	auto const line_end = output.find('\n', line_begin);
	auto line = output.substr(
		line_begin,
		line_end == std::string_view::npos ? output.size() - line_begin : line_end - line_begin);
	if (!line.empty() && line.back() == '\r') {
		line.remove_suffix(1);
	}
	auto const first_space = line.find(' ');
	if (first_space == std::string_view::npos) {
		return std::nullopt;
	}
	auto const code_begin = line.find_first_not_of(' ', first_space);
	if (code_begin == std::string_view::npos) {
		return std::nullopt;
	}
	auto const code_end = line.find(' ', code_begin);
	return std::string{
		line.substr(code_begin, code_end == std::string_view::npos ? line.size() - code_begin : code_end - code_begin)};
}

} // namespace
TEST_CASE(
	"ext/curl: HTTPS GET /ping returns 200 with JSON body") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [code, body] = fx.curl_https("/ping");
	REQUIRE(code == 0);
	REQUIRE(body == R"({"ok":true})");
}
TEST_CASE(
	"ext/curl: HTTPS GET with path param echoes name") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [code, body] = fx.curl_https("/hello/conflux");
	REQUIRE(code == 0);
	REQUIRE(body == "hello conflux");
}
TEST_CASE(
	"ext/curl: HTTPS POST body is echoed") {
	conflux::tests::HttpsServerFixture fx{conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd_retry(
		std::format(
			"curl -sk --http1.1 --max-time 5 -X POST -d 'hello world' "
			"https://127.0.0.1:{}/echo",
			fx.port()));
	REQUIRE(code == 0);
	REQUIRE(body == "hello world");
}
TEST_CASE(
	"ext/curl: HTTPS unknown route returns 404") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [code, status] = fx.curl_https_status("/does-not-exist");
	REQUIRE(code == 0);
	REQUIRE(status == "404");
}
TEST_CASE(
	"ext/curl: HTTPS and HTTP on same port both work") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [tls_code, tls_body] = fx.curl_https("/ping");
	REQUIRE(tls_code == 0);
	REQUIRE(tls_body == R"({"ok":true})");
	auto [plain_code, plain_body] = fx.curl_http("/ping");
	REQUIRE(plain_code == 0);
	REQUIRE(plain_body == R"({"ok":true})");
}
TEST_CASE(
	"ext/curl: HTTPS static file mmap fallback sends full body") {
	char dir_template[] = "/tmp/conflux_tls_static_XXXXXX";
	char *const dir_ptr = ::mkdtemp(dir_template);
	REQUIRE(dir_ptr != nullptr);
	std::string const dir{dir_ptr};
	struct Cleanup {
		std::string path;
		~Cleanup() {
			std::error_code ec;
			(void)std::filesystem::remove_all(path, ec);
		}
	} cleanup{dir};
	std::string const body(256UL * 1024, 'M');
	{
		std::ofstream out{dir + "/large.bin", std::ios::binary};
		out << body;
	}

	conflux::http::Config cfg = conflux::http::Config::test();
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	conflux::http::Router router;
	router.serve_static("/static", dir);

	conflux::tests::HttpsServerFixture const fx{cfg, std::move(router)};
	auto [code, got] = conflux::tests::run_cmd_retry(
		std::format("curl -sk --http1.1 --max-time 5 https://127.0.0.1:{}/static/large.bin", fx.port()));

	REQUIRE(code == 0);
	REQUIRE(got == body);
}
TEST_CASE(
	"ext/curl: TLS 1.2 is accepted") {
	conflux::tests::HttpsServerFixture fx{conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd(
		std::format(
			"curl -skS --http1.1 --tlsv1.2 --tls-max 1.2 "
			"--connect-timeout 1 --max-time 5 "
			"https://127.0.0.1:{}/ping 2>&1",
			fx.port()));
	INFO("curl exit=" << code << " output=" << body);
	REQUIRE(code == 0);
	REQUIRE(body == R"({"ok":true})");
}

TEST_CASE(
	"ext/curl: TLS 1.3 is accepted") {
	conflux::tests::HttpsServerFixture fx{conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd(
		std::format(
			"curl -skS --http1.1 --tlsv1.3 --tls-max 1.3 "
			"--connect-timeout 1 --max-time 5 "
			"https://127.0.0.1:{}/ping 2>&1",
			fx.port()));
	INFO("curl exit=" << code << " output=" << body);
	REQUIRE(code == 0);
	REQUIRE(body == R"({"ok":true})");
}
TEST_CASE(
	"ext/openssl: s_client GET /ping returns 200 OK") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto resp = fx.sclient_get("/ping");
	REQUIRE(first_http_status_code(resp) == "200");
	REQUIRE(resp.find(R"({"ok":true})") != std::string::npos);
}
TEST_CASE(
	"ext/openssl: s_client negotiates TLS and server does not crash") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto resp = fx.sclient_get("/ping");
	REQUIRE(!resp.empty());
	auto [code, body] = fx.curl_https("/ping");
	REQUIRE(code == 0);
	REQUIRE(body == R"({"ok":true})");
}
TEST_CASE(
	"ext/openssl: s_client GET path param echoes correctly") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto resp = fx.sclient_get("/hello/tls");
	REQUIRE(first_http_status_code(resp) == "200");
	REQUIRE(resp.find("hello tls") != std::string::npos);
}
TEST_CASE(
	"ext/openssl: multiple sequential s_client connections all succeed") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	for (int i = 0; i < 5; ++i) {
		auto resp = fx.sclient_get("/ping");
		REQUIRE(first_http_status_code(resp) == "200");
		REQUIRE(resp.find(R"({"ok":true})") != std::string::npos);
	}
}
TEST_CASE(
	"ext/curl: TLS 1.1 is rejected") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd(
		std::format("curl -sk --tls-max 1.1 --tlsv1.1 --max-time 5 https://127.0.0.1:{}/ping 2>&1", fx.port()));
	// curl exits non-zero on handshake failure; body may be empty or an error message.
	REQUIRE(code != 0);
	REQUIRE(body.find(R"({"ok":true})") == std::string::npos);
}
TEST_CASE(
	"tls/bad-client: malformed ClientHello is rejected and server stays healthy") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto fd = connect_loopback(fx.port());
	std::array<unsigned char, 27> const garbage{0x16, 0x03, 0x03, 0x00, 0x20, 'n', 'o', 't', '-',
												'a',  '-',  'r',  'e',  'a',  'l', '-', 'c', 'l',
												'i',  'e',  'n',  't',  'h',  'e', 'l', 'l', 'o'};
	REQUIRE(::send(fd.get(), garbage.data(), garbage.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(garbage.size()));
	::shutdown(fd.get(), SHUT_WR);
	auto const response = read_available(fd.get(), std::chrono::milliseconds{500});
	CHECK(response.find("HTTP/") == std::string::npos);
	require_https_ping_ok(fx);
}

TEST_CASE(
	"tls/bad-client: early close during handshake does not poison listener") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	for (int i = 0; i < 8; ++i) {
		auto fd = connect_loopback(fx.port());
		fd.reset();
	}
	require_https_ping_ok(fx);
}

TEST_CASE(
	"tls/bad-client: sniff timeout closes idle pre-handshake connection") {
	conflux::http::Config cfg = conflux::http::Config::test();
	cfg.tls_sniff_timeout_ms = 50;
	conflux::tests::HttpsServerFixture const fx{cfg, conflux::tests::make_external_test_router()};
	auto fd = connect_loopback(fx.port());
	auto const response = read_available(fd.get(), std::chrono::milliseconds{750});
	CHECK(response.empty());
	require_https_ping_ok(fx);
}

TEST_CASE(
	"tls/alpn: http/1.1 ALPN is accepted") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd_retry(
		std::format(
			"printf 'GET /ping HTTP/1.1\\r\\nHost: localhost\\r\\nConnection: close\\r\\n\\r\\n' | "
			"openssl s_client -connect 127.0.0.1:{} -alpn http/1.1 -quiet -ign_eof 2>/dev/null",
			fx.port()));
	INFO("openssl exit=" << code << " output=" << body);
	REQUIRE(first_http_status_code(body) == "200");
	REQUIRE(body.find(R"({"ok":true})") != std::string::npos);
}

TEST_CASE(
	"tls/alpn: unknown ALPN falls back without breaking h1") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd_retry(
		std::format(
			"printf 'GET /ping HTTP/1.1\\r\\nHost: localhost\\r\\nConnection: close\\r\\n\\r\\n' | "
			"openssl s_client -connect 127.0.0.1:{} -alpn conflux-unknown -quiet -ign_eof 2>/dev/null",
			fx.port()));
	INFO("openssl exit=" << code << " output=" << body);
	REQUIRE(first_http_status_code(body) == "200");
	REQUIRE(body.find(R"({"ok":true})") != std::string::npos);
}

#if CONFLUX_HAS_HTTP3
TEST_CASE(
	"tls/alpn: TCP TLS does not negotiate h3") {
	conflux::http::Config cfg = conflux::http::Config::test();
	cfg.http3.enabled = true;
	conflux::tests::HttpsServerFixture const fx{cfg, conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd_retry(
		std::format("openssl s_client -connect 127.0.0.1:{} -alpn h3 </dev/null 2>&1", fx.port()));
	INFO("openssl exit=" << code << " output=" << body);
	REQUIRE(body.find("ALPN protocol: h3") == std::string::npos);
}
#endif
TEST_CASE(
	"ext/curl: SSE streams all events and closes") {
	conflux::http::Router r;
	r.sse("/events", [](Request const &, std::shared_ptr<conflux::http::SseChannel> const &ch) {
		auto _ = ch->send("data: alpha\n\n");
		CONFLUX_DISCARD(ch->send("data: beta\n\n"));
		ch->close();
	});
	conflux::tests::HttpsServerFixture const fx{std::move(r)};
	auto [code, body] = conflux::tests::run_cmd_retry(
		std::format("curl -sk --http1.1 -N --max-time 5 https://127.0.0.1:{}/events", fx.port()));
	INFO(std::format("code: {}, body: {}", code, body));
	REQUIRE(code == 0);
	REQUIRE(body == "data: alpha\n\ndata: beta\n\n");
}
TEST_CASE(
	"ext/curl: SSE send_event delivers typed event") {
	conflux::http::Router r;
	r.sse("/typed", [](Request const &, std::shared_ptr<conflux::http::SseChannel> const &ch) {
		auto _ = ch->send_event("update", "payload42");
		ch->close();
	});
	conflux::tests::HttpsServerFixture const fx{std::move(r)};
	auto [code, body] = conflux::tests::run_cmd_retry(
		std::format("curl -sk --http1.1 -N --max-time 5 https://127.0.0.1:{}/typed", fx.port()));
	INFO(std::format("code: {}, body: {}", code, body));
	REQUIRE(code == 0);
	REQUIRE(body == "event: update\ndata: payload42\n\n");
}
