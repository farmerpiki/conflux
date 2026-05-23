// Plain TU — not a module unit.
#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work;
import conflux.uring;
import conflux.uring.completion;
import conflux.socket_io;
import conflux.net.http.client;
import conflux.net.async_client;

namespace {

namespace chttp = conflux::http;
namespace root = conflux::work::root;
using conflux::uring::CqeFlags;

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}

void dispatch_available(
	::io_uring &ring,
	CompletionTable &completions) {
	std::array<::io_uring_cqe *, 64> batch{};
	for (;;) {
		unsigned const n = ::io_uring_peek_batch_cqe(&ring, batch.data(), static_cast<unsigned>(batch.size()));
		if (n == 0) {
			break;
		}
		for (unsigned i = 0; i < n; ++i) {
			auto const *cqe = batch[static_cast<std::size_t>(i)];
			auto const ud = cqe->user_data;
			completions.dispatch(
				static_cast<std::uint32_t>(ud & 0xFFFFFFFFU),
				static_cast<std::uint32_t>(ud >> 32U),
				cqe->res,
				CqeFlags{cqe->flags});
		}
		::io_uring_cq_advance(&ring, n);
	}
}

void pump_once(
	::io_uring &ring,
	CompletionTable &completions,
	std::chrono::milliseconds wait = std::chrono::milliseconds{25}) {
	::io_uring_cqe *cqe = nullptr;
	auto const sec = std::chrono::duration_cast<std::chrono::seconds>(wait);
	__kernel_timespec ts{
		.tv_sec = sec.count(),
		.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(wait - sec).count(),
	};
	int const rc = ::io_uring_submit_and_wait_timeout(&ring, &cqe, 1, &ts, nullptr);
	if (rc != -ETIME && rc != -EINTR && rc < 0) {
		throw std::runtime_error{std::format("io_uring_submit_and_wait_timeout failed: {}", rc)};
	}
	dispatch_available(ring, completions);
}

struct RingFixture {
	::io_uring ring{};
	CompletionTable completions{};
	SocketTaskRing task_ring;
	bool ring_ok{false};

	RingFixture()
		: task_ring{SocketRawRing{&ring}, completions, [](std::uint32_t s, std::uint32_t g) noexcept {
						return pack_ud(s, g);
					}} {}
	~RingFixture() {
		if (ring_ok) {
			::io_uring_queue_exit(&ring);
		}
	}
	RingFixture(RingFixture const &) = delete;
	RingFixture &operator =(RingFixture const &) = delete;

	static std::unique_ptr<RingFixture> make(
		unsigned entries = 128) {
		auto fx = std::make_unique<RingFixture>();
		if (::io_uring_queue_init(entries, &fx->ring, 0) < 0) {
			return {};
		}
		fx->ring_ok = true;
		return fx;
	}

	template<typename Pred>
	bool pump_until(
		Pred pred,
		std::chrono::milliseconds budget = std::chrono::seconds{3}) {
		auto const deadline = std::chrono::steady_clock::now() + budget;
		while (!pred()) {
			if (std::chrono::steady_clock::now() >= deadline) {
				return false;
			}
			pump_once(ring, completions);
		}
		return true;
	}

	template<typename T>
	root::Outcome<T> run_to_outcome(
		root::Task<T> task,
		std::chrono::milliseconds budget = std::chrono::seconds{5}) {
		struct Slot {
			std::atomic_flag done{};
			std::exception_ptr err{};
			std::optional<root::Outcome<T>> outcome{};
		};
		auto slot = std::make_shared<Slot>();
		auto jh = std::make_shared<root::TaskJoinHandle<T>>(root::into_join_handle(std::move(task)));
		jh->control().set_on_ready_or_run([slot, jh]() noexcept {
			try {
				slot->outcome.emplace(root::blocking_join(std::move(*jh)));
			} catch (...) { slot->err = std::current_exception(); }
			slot->done.test_and_set(std::memory_order_release);
		});
		auto const deadline = std::chrono::steady_clock::now() + budget;
		while (!slot->done.test(std::memory_order_acquire)) {
			if (std::chrono::steady_clock::now() >= deadline) {
				throw std::runtime_error{"async client e2e budget exhausted"};
			}
			pump_once(ring, completions);
		}
		if (slot->err) {
			std::rethrow_exception(slot->err);
		}
		return std::move(*slot->outcome);
	}
};

std::unique_ptr<RingFixture> require_ring_fixture() {
	auto fx = RingFixture::make();
	INFO("conflux async client cancellation tests require io_uring_queue_init");
	REQUIRE(fx != nullptr);
	return fx;
}

class ScriptedTcpServer {
public:
	enum class Mode : std::uint8_t {
		idle_after_accept,
		idle_after_request,
		partial_body_then_idle,
	};

	explicit ScriptedTcpServer(
		Mode mode)
		: mode_{mode} {
		listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
		if (listen_fd_ < 0) {
			throw std::runtime_error{"socket failed"};
		}
		int const one = 1;
		::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		::sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sa.sin_port = 0;
		if (::bind(listen_fd_, reinterpret_cast<::sockaddr *>(&sa), sizeof(sa)) < 0) {
			throw std::runtime_error{"bind failed"};
		}
		if (::listen(listen_fd_, 1) < 0) {
			throw std::runtime_error{"listen failed"};
		}
		::sockaddr_in bound{};
		socklen_t len = sizeof(bound);
		if (::getsockname(listen_fd_, reinterpret_cast<::sockaddr *>(&bound), &len) < 0) {
			throw std::runtime_error{"getsockname failed"};
		}
		port_ = ntohs(bound.sin_port);
		thread_ = std::jthread{[this](std::stop_token st) { run(st); }};
	}
	~ScriptedTcpServer() {
		thread_.request_stop();
		if (client_fd_.load(std::memory_order_acquire) >= 0) {
			::shutdown(client_fd_.load(std::memory_order_relaxed), SHUT_RDWR);
		}
		if (listen_fd_ >= 0) {
			::shutdown(listen_fd_, SHUT_RDWR);
			::close(listen_fd_);
		}
	}
	ScriptedTcpServer(ScriptedTcpServer const &) = delete;
	ScriptedTcpServer &operator =(ScriptedTcpServer const &) = delete;

	[[nodiscard]] std::uint16_t port() const noexcept { return port_; }
	[[nodiscard]] bool accepted() const noexcept { return accepted_.load(std::memory_order_acquire); }
	[[nodiscard]] bool request_seen() const noexcept { return request_seen_.load(std::memory_order_acquire); }
	[[nodiscard]] bool body_started() const noexcept { return body_started_.load(std::memory_order_acquire); }

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
		int rcv = 1024;
		::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
		accepted_.store(true, std::memory_order_release);

		if (mode_ == Mode::idle_after_accept) {
			idle(st);
			::close(fd);
			return;
		}

		std::string req;
		std::array<char, 4096> buf{};
		while (!st.stop_requested() && req.find("\r\n\r\n") == std::string::npos) {
			ssize_t const n = ::recv(fd, buf.data(), buf.size(), 0);
			if (n > 0) {
				req.append(buf.data(), static_cast<std::size_t>(n));
				continue;
			}
			if (n == 0) {
				::close(fd);
				return;
			}
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				::close(fd);
				return;
			}
		}
		request_seen_.store(true, std::memory_order_release);
		if (mode_ == Mode::partial_body_then_idle) {
			std::string_view wire = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\nab";
			(void)::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL);
			body_started_.store(true, std::memory_order_release);
		}
		idle(st);
		::close(fd);
	}

	static void idle(
		std::stop_token st) noexcept {
		while (!st.stop_requested()) {
			std::this_thread::sleep_for(std::chrono::milliseconds{20});
		}
	}

	Mode mode_{};
	int listen_fd_{-1};
	std::uint16_t port_{0};
	std::atomic<int> client_fd_{-1};
	std::atomic_bool accepted_{false};
	std::atomic_bool request_seen_{false};
	std::atomic_bool body_started_{false};
	std::jthread thread_{};
};

chttp::HttpClient make_client(
	std::chrono::milliseconds timeout) {
	chttp::HttpTimeouts timeouts{};
	timeouts.resolve = timeout;
	timeouts.connect = timeout;
	timeouts.tls = timeout;
	timeouts.write = timeout;
	timeouts.first_byte = timeout;
	timeouts.between_bytes = timeout;
	chttp::HttpClientOptions opts{};
	opts.default_timeouts = timeouts;
	opts.max_body_bytes = 128 * 1024 * 1024;
	opts.max_buffered_bytes = 128 * 1024 * 1024;
	return chttp::HttpClient{std::move(opts)};
}

[[nodiscard]] chttp::ClientRequest get_request(
	std::uint16_t port,
	std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
	chttp::HttpTimeouts timeouts{};
	timeouts.resolve = timeout;
	timeouts.connect = timeout;
	timeouts.tls = timeout;
	timeouts.write = timeout;
	timeouts.first_byte = timeout;
	timeouts.between_bytes = timeout;
	return chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/", port)).timeouts(timeouts).build();
}

} // namespace

TEST_CASE(
	"http async client: cancellation before connect completion is terminal",
	"[http][client][cancel][connect][uring]") {
	auto fx = require_ring_fixture();
	auto client = make_client(std::chrono::seconds{5});
	auto req = get_request(9, std::chrono::seconds{5});
	auto task = chttp::async_send(client, fx->task_ring, req);
	task.cancel();
	auto out = fx->run_to_outcome(std::move(task));
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"http async client: cancellation reaches pending request write or response wait",
	"[http][client][cancel][write][uring]") {
	auto fx = require_ring_fixture();
	ScriptedTcpServer server{ScriptedTcpServer::Mode::idle_after_accept};
	auto client = make_client(std::chrono::seconds{10});
	std::string body(32 * 1024 * 1024, 'x');
	auto req = chttp::ClientRequest::post(std::format("http://127.0.0.1:{}/upload", server.port()))
				   .header("Content-Type", "application/octet-stream")
				   .body(body)
				   .build();
	auto task = chttp::async_send(client, fx->task_ring, req);
	REQUIRE(fx->pump_until([&] { return server.accepted(); }));
	for (int i = 0; i < 4; ++i) {
		pump_once(fx->ring, fx->completions);
	}
	task.cancel();
	auto out = fx->run_to_outcome(std::move(task), std::chrono::seconds{10});
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"http async client: first-byte timeout reports response-header phase",
	"[http][client][timeout][headers][uring]") {
	auto fx = require_ring_fixture();
	ScriptedTcpServer server{ScriptedTcpServer::Mode::idle_after_request};
	auto client = make_client(std::chrono::milliseconds{75});
	auto req = get_request(server.port(), std::chrono::milliseconds{75});
	auto task = chttp::async_send(client, fx->task_ring, req);
	auto out = fx->run_to_outcome(std::move(task), std::chrono::seconds{5});
	REQUIRE(out.is_success());
	auto result = std::move(out).success().value;
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error().kind == chttp::HttpErrorKind::read);
	CHECK(result.error().phase == chttp::HttpPhase::first_byte);
}

TEST_CASE(
	"http async client: cancellation while waiting for response headers is terminal",
	"[http][client][cancel][headers][uring]") {
	auto fx = require_ring_fixture();
	ScriptedTcpServer server{ScriptedTcpServer::Mode::idle_after_request};
	auto client = make_client(std::chrono::seconds{10});
	auto req = get_request(server.port(), std::chrono::seconds{10});
	auto task = chttp::async_send(client, fx->task_ring, req);
	REQUIRE(fx->pump_until([&] { return server.request_seen(); }));
	task.cancel();
	auto out = fx->run_to_outcome(std::move(task), std::chrono::seconds{10});
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"http async client: between-bytes timeout reports response-body phase",
	"[http][client][timeout][body][uring]") {
	auto fx = require_ring_fixture();
	ScriptedTcpServer server{ScriptedTcpServer::Mode::partial_body_then_idle};
	auto client = make_client(std::chrono::milliseconds{75});
	auto req = get_request(server.port(), std::chrono::milliseconds{75});
	auto task = chttp::async_send(client, fx->task_ring, req);
	auto out = fx->run_to_outcome(std::move(task), std::chrono::seconds{5});
	REQUIRE(out.is_success());
	auto result = std::move(out).success().value;
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error().kind == chttp::HttpErrorKind::read);
	CHECK(result.error().phase == chttp::HttpPhase::between_bytes);
}

TEST_CASE(
	"http async client: cancellation while waiting for response body is terminal",
	"[http][client][cancel][body][uring]") {
	auto fx = require_ring_fixture();
	ScriptedTcpServer server{ScriptedTcpServer::Mode::partial_body_then_idle};
	auto client = make_client(std::chrono::seconds{10});
	auto req = get_request(server.port(), std::chrono::seconds{10});
	auto task = chttp::async_send(client, fx->task_ring, req);
	REQUIRE(fx->pump_until([&] { return server.body_started(); }));
	task.cancel();
	auto out = fx->run_to_outcome(std::move(task), std::chrono::seconds{10});
	CHECK(out.is_cancelled());
}
