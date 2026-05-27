// Plain TU — not a module unit. std::thread lambdas + raw POSIX helpers.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http.static_files;
import conflux.net.http_server;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;
namespace {

struct FdGuard {
	int fd{-1};
	explicit FdGuard(
		int f = -1) noexcept
		: fd{f} {}
	~FdGuard() {
		if (fd >= 0) {
			::close(fd);
		}
	}
	FdGuard(FdGuard const &) = delete;
	FdGuard &operator =(FdGuard const &) = delete;
	FdGuard(
		FdGuard &&o) noexcept
		: fd{std::exchange(o.fd, -1)} {}
	FdGuard &operator =(
		FdGuard &&o) noexcept {
		if (this != &o) {
			if (fd >= 0) {
				::close(fd);
			}
			fd = std::exchange(o.fd, -1);
		}
		return *this;
	}
	[[nodiscard]] int get() const noexcept { return fd; }
	[[nodiscard]] int release() noexcept { return std::exchange(fd, -1); }
};

struct StaticDir {
	std::string path;
	explicit StaticDir(
		char const *pattern = "/tmp/conflux_chaos_static_XXXXXX") {
		path = pattern;
		REQUIRE(::mkdtemp(path.data()) != nullptr);
	}
	~StaticDir() {
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
	StaticDir(StaticDir const &) = delete;
	StaticDir &operator =(StaticDir const &) = delete;
	void write(
		std::string_view name,
		std::string_view content) const {
		auto full = path + "/" + std::string{name};
		FdGuard const fd{::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)};
		REQUIRE(fd.get() >= 0);
		std::size_t off = 0;
		while (off < content.size()) {
			auto const n = ::write(fd.get(), content.data() + off, content.size() - off);
			REQUIRE(n > 0);
			off += static_cast<std::size_t>(n);
		}
	}
};

[[nodiscard]] int count_open_fds() noexcept {
	int n = 0;
	auto *d = ::opendir("/proc/self/fd");
	if (d == nullptr) {
		return -1;
	}
	while (::readdir(d) != nullptr) {
		++n;
	}
	::closedir(d);
	return n - 3;
}

[[nodiscard]] FdGuard connect_raw(
	std::uint16_t port) {
	FdGuard fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
	REQUIRE(fd.get() >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	return fd;
}

void send_all(
	int fd,
	std::string_view data) {
	std::size_t off = 0;
	while (off < data.size()) {
		auto const n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
		REQUIRE(n > 0);
		off += static_cast<std::size_t>(n);
	}
}

void reset_connection(
	FdGuard &fd) {
	linger lin{.l_onoff = 1, .l_linger = 0};
	(void)::setsockopt(fd.get(), SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
	int const raw = fd.release();
	if (raw >= 0) {
		::close(raw);
	}
}

std::string_view response_body(
	std::string_view response) {
	auto const header_end = response.find("\r\n\r\n");
	if (header_end == std::string_view::npos) {
		return {};
	}
	return response.substr(header_end + 4);
}

[[nodiscard]] Config chaos_config() {
	Config cfg = mw_config();
	cfg.ring_entries = 128;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	return cfg;
}

std::atomic<HttpServer *> g_sigterm_server{};

void sigterm_shutdown_handler(
	int) noexcept {
	if (auto *srv = g_sigterm_server.load(std::memory_order_acquire); srv != nullptr) {
		srv->request_shutdown();
	}
}

struct SignalGuard {
	using Handler = void (*)(int);
	int signo{};
	Handler previous{};
	SignalGuard(
		int sig,
		Handler handler)
		: signo{sig}
		, previous{::signal(sig, handler)} {}
	~SignalGuard() { (void)::signal(signo, previous); }
	SignalGuard(SignalGuard const &) = delete;
	SignalGuard &operator =(SignalGuard const &) = delete;
};

} // namespace

TEST_CASE(
	"chaos resource: repeated HTTP start/stop preserves fd budget",
	"[chaos][resource][http]") {
	int const before = count_open_fds();
	REQUIRE(before > 0);

	for (int i = 0; i < 6; ++i) {
		Router router;
		router.get("/ping", [](Request const &) { return Response::text("pong"); });
		{
			ScopedTestServer srv{chaos_config(), std::move(router)};
			auto resp = http_get_on(srv.port(), "/ping", "Connection: close\r\n");
			REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
			srv.stop();
		}
	}

	CHECK(count_open_fds() == before);
}

TEST_CASE(
	"chaos resource: static send survives abrupt peer reset flood",
	"[chaos][resource][http][static]") {
	StaticDir const dir;
	dir.write("big.bin", std::string(512UL * 1024, 'x'));
	dir.write("ok.txt", "ok");

	Router router;
	router.serve_static("/static", dir.path);
	ScopedTestServer srv{chaos_config(), std::move(router)};

	for (int i = 0; i < 32; ++i) {
		auto fd = connect_raw(srv.port());
		send_all(fd.get(), "GET /static/big.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
		reset_connection(fd);
	}

	std::this_thread::sleep_for(std::chrono::milliseconds{200});
	auto resp = http_get_on(srv.port(), "/static/ok.txt", "Connection: close\r\n");
	CHECK(resp.starts_with("HTTP/1.1 200 OK"));
	CHECK(response_body(resp) == "ok");
}

TEST_CASE(
	"chaos resource: SIGTERM handler can stop server during active static send",
	"[chaos][resource][http][static]") {
	StaticDir const dir;
	dir.write("large.bin", std::string(2UL * 1024 * 1024, 'z'));

	Router router;
	router.serve_static("/static", dir.path);
	auto server = std::make_shared<HttpServer>(chaos_config(), std::move(router));
	SignalGuard const sigterm{SIGTERM, sigterm_shutdown_handler};
	g_sigterm_server.store(server.get(), std::memory_order_release);

	std::atomic_bool run_returned{false};
	std::thread server_thread{[&server, &run_returned] {
		(void)server->run();
		run_returned.store(true, std::memory_order_release);
	}};
	auto const port = server->port();
	wait_for_server(port);

	auto client = connect_raw(port);
	timeval tv{.tv_sec = 2, .tv_usec = 0};
	(void)::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	send_all(client.get(), "GET /static/large.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");

	std::array<char, 1024> buf{};
	auto const n = ::recv(client.get(), buf.data(), buf.size(), 0);
	REQUIRE(n > 0);

	::raise(SIGTERM);
	reset_connection(client);
	if (server_thread.joinable()) {
		server_thread.join();
	}
	g_sigterm_server.store(nullptr, std::memory_order_release);
	CHECK(run_returned.load(std::memory_order_acquire));
}
