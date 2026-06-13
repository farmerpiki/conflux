module;
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#if CONFLUX_HAS_TLS
	#include <openssl/bio.h>
	#include <openssl/buffer.h>
	#include <openssl/evp.h>
	#include <openssl/pem.h>
	#include <openssl/rsa.h>
	#include <openssl/x509.h>
#endif

export module conflux.tests.support;

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http_server;
import conflux.net.router;
import conflux.net.vhost;

namespace {

#if CONFLUX_HAS_TLS
struct TestBioDeleter {
	void operator ()(
		BIO *p) const noexcept {
		BIO_free(p);
	}
};
struct TestEvpPkeyDeleter {
	void operator ()(
		EVP_PKEY *p) const noexcept {
		EVP_PKEY_free(p);
	}
};
struct TestEvpPkeyCtxDeleter {
	void operator ()(
		EVP_PKEY_CTX *p) const noexcept {
		EVP_PKEY_CTX_free(p);
	}
};
struct TestX509Deleter {
	void operator ()(
		X509 *p) const noexcept {
		X509_free(p);
	}
};

using TestUniqueBio = std::unique_ptr<BIO, TestBioDeleter>;
using TestUniqueEvpPkey = std::unique_ptr<EVP_PKEY, TestEvpPkeyDeleter>;
using TestUniqueEvpPkeyCtx = std::unique_ptr<EVP_PKEY_CTX, TestEvpPkeyCtxDeleter>;
using TestUniqueX509 = std::unique_ptr<X509, TestX509Deleter>;

[[nodiscard]] TestUniqueEvpPkey make_test_private_key() {
	TestUniqueEvpPkeyCtx ctx{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
	if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0) {
		throw std::runtime_error{"test TLS keygen init failed"};
	}
	EVP_PKEY *raw = nullptr;
	if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0 || raw == nullptr) {
		throw std::runtime_error{"test TLS keygen failed"};
	}
	return TestUniqueEvpPkey{raw};
}

[[nodiscard]] TestUniqueX509 make_test_certificate(
	EVP_PKEY *key) {
	TestUniqueX509 cert{X509_new()};
	if (!cert) {
		throw std::runtime_error{"test TLS X509_new failed"};
	}
	if (X509_set_version(cert.get(), 2) != 1) {
		throw std::runtime_error{"test TLS X509_set_version failed"};
	}
	if (ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1) {
		throw std::runtime_error{"test TLS serial failed"};
	}
	if (X509_gmtime_adj(X509_get_notBefore(cert.get()), 0) == nullptr
		|| X509_gmtime_adj(X509_get_notAfter(cert.get()), 24 * 60 * 60) == nullptr) {
		throw std::runtime_error{"test TLS validity failed"};
	}
	if (X509_set_pubkey(cert.get(), key) != 1) {
		throw std::runtime_error{"test TLS pubkey failed"};
	}
	X509_NAME *name = X509_get_subject_name(cert.get());
	auto const *localhost = reinterpret_cast<unsigned char const *>("localhost");
	if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, localhost, -1, -1, 0) != 1) {
		throw std::runtime_error{"test TLS subject failed"};
	}
	if (X509_set_issuer_name(cert.get(), name) != 1) {
		throw std::runtime_error{"test TLS issuer failed"};
	}
	if (X509_sign(cert.get(), key, EVP_sha256()) <= 0) {
		throw std::runtime_error{"test TLS cert sign failed"};
	}
	return cert;
}

[[nodiscard]] std::string bio_string(
	BIO *bio) {
	BUF_MEM *mem = nullptr;
	BIO_get_mem_ptr(bio, &mem);
	if (mem == nullptr || mem->data == nullptr) {
		return {};
	}
	return std::string{mem->data, mem->length};
}

[[nodiscard]] std::pair<std::string, std::string> make_test_cert_pem_pair() {
	auto key = make_test_private_key();
	auto cert = make_test_certificate(key.get());
	TestUniqueBio cert_bio{BIO_new(BIO_s_mem())};
	TestUniqueBio key_bio{BIO_new(BIO_s_mem())};
	if (!cert_bio || !key_bio) {
		throw std::runtime_error{"test TLS memory BIO failed"};
	}
	if (PEM_write_bio_X509(cert_bio.get(), cert.get()) != 1
		|| PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
		throw std::runtime_error{"test TLS PEM write failed"};
	}
	return {bio_string(cert_bio.get()), bio_string(key_bio.get())};
}
#endif

} // namespace

export namespace conflux::tests {

std::pair<std::string, std::string> const &cached_test_cert() {
#if CONFLUX_HAS_TLS
	static std::pair<std::string, std::string> const bytes = make_test_cert_pem_pair();
	return bytes;
#else
	static std::pair<std::string, std::string> const bytes{};
	return bytes;
#endif
}

void configure_test_tls(
	conflux::http::Config &cfg) {
#if CONFLUX_HAS_TLS
	auto const &[cert_pem, key_pem] = cached_test_cert();
	cfg.cert_pem = cert_pem;
	cfg.key_pem = key_pem;
#else
	std::ignore = cfg;
	throw std::runtime_error{"TLS support is disabled"};
#endif
}

enum class SocketReadEnd {
	eof,
	reset,
	timeout,
	error,
};

struct ReadUntilCloseResult {
	std::string bytes;
	SocketReadEnd end = SocketReadEnd::eof;
	int error = 0;

	[[nodiscard]] bool closed() const noexcept;
};

[[nodiscard]] bool is_socket_closed(
	SocketReadEnd end) noexcept {
	return end == SocketReadEnd::eof || end == SocketReadEnd::reset;
}

bool ReadUntilCloseResult::closed() const noexcept {
	return is_socket_closed(end);
}

[[nodiscard]] SocketReadEnd classify_recv_end(
	ssize_t n,
	int error) noexcept {
	if (n == 0) {
		return SocketReadEnd::eof;
	}
	if (n < 0 && error == ECONNRESET) {
		return SocketReadEnd::reset;
	}
	if (n < 0 && error == EAGAIN) {
		return SocketReadEnd::timeout;
	}
#if EWOULDBLOCK != EAGAIN
	if (n < 0 && error == EWOULDBLOCK) {
		return SocketReadEnd::timeout;
	}
#endif
	return SocketReadEnd::error;
}

[[nodiscard]] ReadUntilCloseResult read_until_close_with_state(
	int fd) {
	ReadUntilCloseResult result;
	std::array<char, 4096> buf{};
	for (;;) {
		errno = 0;
		auto const n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n <= 0) {
			result.error = errno;
			result.end = classify_recv_end(n, result.error);
			break;
		}
		result.bytes.append(buf.data(), static_cast<std::size_t>(n));
	}
	return result;
}

[[nodiscard]] SocketReadEnd recv_close_state(
	int fd,
	int flags = 0) {
	char byte{};
	errno = 0;
	auto const n = ::recv(fd, &byte, 1, flags);
	return classify_recv_end(n, errno);
}

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
	[[nodiscard]] std::string read_until_close() const { return read_until_close_with_state(fd_).bytes; }
	[[nodiscard]] std::string read_one_response() const { return ::conflux::tests::read_one_response(fd_); }
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
	std::vector<std::shared_ptr<conflux::http::HttpServer>> servers_;
	std::vector<std::thread> threads_;

public:
	std::uint16_t start(
		conflux::http::Config const &cfg,
		conflux::http::Router router) {
		// TLS server probing in wait_for_server() triggers SIGPIPE without this.
		(void)::signal(SIGPIPE, SIG_IGN);
		auto srv = std::make_shared<conflux::http::HttpServer>(cfg, std::move(router));
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
		conflux::http::Config const &cfg,
		conflux::http::VHostRouter vhost_router) {
		(void)::signal(SIGPIPE, SIG_IGN);
		auto srv = std::make_shared<conflux::http::HttpServer>(cfg, std::move(vhost_router));
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
	std::shared_ptr<conflux::http::HttpServer> server_;
	std::thread thread_;

public:
	ScopedTestServer(
		conflux::http::Config const &cfg,
		conflux::http::Router router)
		: server_([&] {
			auto local_cfg = cfg;
			local_cfg.startup_banner = false;
			return std::make_shared<conflux::http::HttpServer>(local_cfg, std::move(router));
		}())
		, thread_([srv = server_] { (void)srv->run(); }) {
		wait_for_server(server_->port());
	}
	[[nodiscard]] std::uint16_t port() const { return server_->port(); }
	[[nodiscard]] conflux::http::HttpServerMetrics metrics() const noexcept { return server_->metrics(); }
	[[nodiscard]] conflux::http::DrainReport drain(
		conflux::http::DrainOptions options = {}) {
		auto report = server_->drain(options);
		if (thread_.joinable()) {
			thread_.join();
		}
		return report;
	}
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
[[nodiscard]] conflux::http::Config mw_config() {
	conflux::http::Config cfg{};
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
	conflux::http::Config const &cfg,
	conflux::http::Router router) {
	return test_servers().start(cfg, std::move(router));
}

} // namespace conflux::tests
