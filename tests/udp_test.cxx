// Plain TU — not a module unit.
#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.uring.completion;
import conflux.work;
import conflux.socket_io;
import conflux.socket_io.coro;
using conflux::IoError;
using conflux::uring::CompletionTable;
using namespace conflux::socket_io;

static_assert(std::same_as<
			  decltype(std::declval<UdpSocket &>().async_recv_from(std::declval<std::span<std::uint8_t>>())),
			  conflux::work::root::JoinTask<UdpRecvResult>>);
static_assert(std::same_as<
			  decltype(std::declval<UdpSocket &>()
						   .async_recv_from(std::declval<std::span<std::uint8_t>>(), std::chrono::milliseconds{})),
			  conflux::work::root::JoinTask<UdpRecvResult>>);

namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}
template<typename T>
T block_on_ring(
	::io_uring *ring,
	CompletionTable &completions,
	conflux::work::root::Task<T> task,
	std::chrono::milliseconds budget = std::chrono::seconds{5}) {
	using namespace conflux::work::root;
	struct Slot {
		std::atomic_flag done{};
		std::exception_ptr err{};
		[[no_unique_address]] std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>> value{};
	};
	auto slot = std::make_shared<Slot>();
	auto jh = std::make_shared<TaskJoinHandle<T>>(into_join_handle(std::move(task)));
	jh->control().set_on_ready_or_run([slot, jh]() noexcept {
		try {
			auto outcome = blocking_join(std::move(*jh));
			if (outcome.is_failure()) {
				slot->err = std::move(outcome).failure().error;
			} else if (outcome.is_cancelled()) {
				slot->err = make_exception_ptr(std::runtime_error{"task cancelled"});
			} else if constexpr (!std::is_void_v<T>) {
				slot->value.emplace(std::move(outcome).success().value);
			}
		} catch (...) { slot->err = std::current_exception(); }
		slot->done.test_and_set(std::memory_order_release);
	});
	auto const deadline = std::chrono::steady_clock::now() + budget;
	while (!slot->done.test(std::memory_order_acquire)) {
		::io_uring_cqe *cqe = nullptr;
		__kernel_timespec ts{.tv_sec = 1, .tv_nsec = 0};
		int const rc = ::io_uring_submit_and_wait_timeout(ring, &cqe, 1, &ts, nullptr);
		if (rc == -ETIME) {
			if (std::chrono::steady_clock::now() > deadline) {
				throw std::runtime_error{"block_on_ring: budget exhausted"};
			}
			continue;
		}
		if (rc == -EINTR) {
			continue;
		}
		if (rc >= 0 && cqe == nullptr) {
			continue;
		}
		std::array<::io_uring_cqe *, 32> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(ring, batch.data(), 32u);
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				auto const *c = batch[static_cast<std::size_t>(i)];
				auto ud = c->user_data;
				completions.dispatch(
					static_cast<std::uint32_t>(ud & 0xFFFFFFFFU),
					static_cast<std::uint32_t>(ud >> 32U),
					c->res,
					conflux::uring::CqeFlags{c->flags});
			}
			::io_uring_cq_advance(ring, n);
			if (slot->done.test(std::memory_order_acquire)) {
				break;
			}
		}
	}
	if (slot->err) {
		rethrow_exception(slot->err);
	}
	if constexpr (!std::is_void_v<T>) {
		return std::move(*slot->value);
	}
}
struct RingFixture {
	::io_uring ring{};
	CompletionTable completions{};
	SocketTaskRing task_ring;
	bool ring_ok{false};
	RingFixture()
		: task_ring{
			  SocketRawRing{&ring},
			  completions,
			  [](std::uint32_t slot, std::uint32_t gen) noexcept -> std::uint64_t { return pack_ud(slot, gen); }} {}
	static std::unique_ptr<RingFixture> make(
		unsigned entries = 64) {
		auto fx = std::make_unique<RingFixture>();
		if (::io_uring_queue_init(entries, &fx->ring, 0) < 0) {
			return {};
		}
		fx->ring_ok = true;
		return fx;
	}
	~RingFixture() {
		if (ring_ok) {
			::io_uring_queue_exit(&ring);
		}
	}
	RingFixture(RingFixture const &) = delete;
	RingFixture &operator =(RingFixture const &) = delete;
	RingFixture(RingFixture &&) = delete;
	RingFixture &operator =(RingFixture &&) = delete;
	template<typename T>
	T run(
		conflux::work::root::Task<T> task,
		std::chrono::milliseconds budget = std::chrono::seconds{5}) {
		return block_on_ring(&ring, completions, std::move(task), budget);
	}
	template<typename T>
	T run(
		conflux::work::root::JoinTask<T> task,
		std::chrono::milliseconds budget = std::chrono::seconds{5}) {
		return block_on_ring(&ring, completions, std::move(task).detach_to_task(), budget);
	}
};
std::unique_ptr<RingFixture> require_ring_fixture(
	unsigned entries = 64) {
	auto fx = RingFixture::make(entries);
	INFO("conflux requires a host that permits io_uring_queue_init");
	REQUIRE(fx != nullptr);
	return fx;
}
std::uint16_t get_local_port(
	int fd) noexcept {
	sockaddr_storage ss{};
	socklen_t len = sizeof(ss);
	if (::getsockname(fd, reinterpret_cast<sockaddr *>(&ss), &len) < 0) {
		return 0;
	}
	if (ss.ss_family == AF_INET) {
		return ntohs(reinterpret_cast<sockaddr_in const *>(&ss)->sin_port);
	}
	if (ss.ss_family == AF_INET6) {
		return ntohs(reinterpret_cast<sockaddr_in6 const *>(&ss)->sin6_port);
	}
	return 0;
}
sockaddr_storage to_storage(
	sockaddr_in const &sa) noexcept {
	sockaddr_storage ss{};
	memcpy(&ss, &sa, sizeof(sa));
	return ss;
}

} // namespace
// ---------------------------------------------------------------------------
// UdpSocket — RAII / ephemeral / valid
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: ephemeral binds to non-zero local port (AF_INET)",
	"[udp]") {
	auto fx = require_ring_fixture();
	auto sock = UdpSocket::ephemeral(fx->task_ring, AF_INET);
	CHECK(sock.valid());
	CHECK(sock.raw_fd() >= 0);
	CHECK(get_local_port(sock.raw_fd()) > 0);
}
TEST_CASE(
	"udp: ephemeral works for AF_INET6",
	"[udp]") {
	auto fx = require_ring_fixture();
	auto sock = UdpSocket::ephemeral(fx->task_ring, AF_INET6);
	CHECK(sock.valid());
	CHECK(get_local_port(sock.raw_fd()) > 0);
}
TEST_CASE(
	"udp: UdpSocket std::move-only — source becomes empty",
	"[udp]") {
	auto fx = require_ring_fixture();
	auto src = UdpSocket::ephemeral(fx->task_ring, AF_INET);
	int const fd = src.raw_fd();
	REQUIRE(fd >= 0);
	auto dst = std::move(src);
	CHECK_FALSE(src.valid());
	CHECK(dst.raw_fd() == fd);
}
// ---------------------------------------------------------------------------
// async_send_to_borrowed + async_recv_from — loopback round-trip
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: send and recv on loopback (AF_INET)",
	"[udp][uring]") {
	auto fx = require_ring_fixture();

	auto recv_sock = UdpSocket::ephemeral(fx->task_ring, AF_INET);
	auto send_sock = UdpSocket::ephemeral(fx->task_ring, AF_INET);

	std::uint16_t const recv_port = get_local_port(recv_sock.raw_fd());
	REQUIRE(recv_port > 0);

	std::array<std::uint8_t, 5> payload{'h', 'e', 'l', 'l', 'o'};
	sockaddr_in dest{};
	dest.sin_family = AF_INET;
	dest.sin_port = htons(recv_port);
	dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t const dest_len = sizeof(dest);

	auto const bytes_sent = fx->run(send_sock.async_send_to_borrowed(
		std::span<std::uint8_t const>{payload.data(), payload.size()},
		to_storage(dest),
		dest_len));
	CHECK(bytes_sent == payload.size());

	std::array<std::uint8_t, 256> rx_buf{};
	auto const rx = fx->run(recv_sock.async_recv_from(std::span<std::uint8_t>{rx_buf.data(), rx_buf.size()}));

	REQUIRE(rx.bytes == payload.size());
	CHECK(memcmp(rx_buf.data(), payload.data(), rx.bytes) == 0);
	CHECK(rx.from_len >= sizeof(::sockaddr_in));
	auto const &from = *reinterpret_cast<::sockaddr_in const *>(&rx.from);
	CHECK(from.sin_family == AF_INET);
	CHECK(from.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
	CHECK(ntohs(from.sin_port) == get_local_port(send_sock.raw_fd()));
}
// ---------------------------------------------------------------------------
// async_recv_from with timeout — fires on idle socket
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: async_recv_from with timeout fires IoError on idle socket",
	"[udp][uring]") {
	auto fx = require_ring_fixture();

	auto sock = UdpSocket::ephemeral(fx->task_ring, AF_INET);

	std::array<std::uint8_t, 256> rx_buf{};
	int err_code = 0;
	bool got_value = false;
	try {
		fx->run(
			sock.async_recv_from(std::span<std::uint8_t>{rx_buf.data(), rx_buf.size()}, std::chrono::milliseconds{50}),
			std::chrono::seconds{2});
		got_value = true;
	} catch (IoError const &e) { err_code = e.code().value(); } catch (...) {
	}

	CHECK_FALSE(got_value);
	CHECK(err_code == ETIMEDOUT);
}
// ---------------------------------------------------------------------------
// async_recv_from with timeout — succeeds when data arrives in time
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: async_recv_from with timeout receives packet that arrives before timeout",
	"[udp][uring]") {
	auto fx = require_ring_fixture();

	auto recv_sock = UdpSocket::ephemeral(fx->task_ring, AF_INET);
	auto send_sock = UdpSocket::ephemeral(fx->task_ring, AF_INET);

	std::uint16_t const recv_port = get_local_port(recv_sock.raw_fd());

	std::array<std::uint8_t, 4> payload{0xDE, 0xAD, 0xBE, 0xEF};
	sockaddr_in dest{};
	dest.sin_family = AF_INET;
	dest.sin_port = htons(recv_port);
	dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t const dest_len = sizeof(dest);

	fx->run(send_sock.async_send_to_borrowed(
		std::span<std::uint8_t const>{payload.data(), payload.size()},
		to_storage(dest),
		dest_len));

	std::array<std::uint8_t, 256> rx_buf{};
	auto const rx = fx->run(
		recv_sock.async_recv_from(
			std::span<std::uint8_t>{rx_buf.data(), rx_buf.size()},
			std::chrono::milliseconds{2000}),
		std::chrono::seconds{3});

	REQUIRE(rx.bytes == payload.size());
	CHECK(memcmp(rx_buf.data(), payload.data(), rx.bytes) == 0);
}
// ---------------------------------------------------------------------------
// IPv6 send + recv round-trip
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: send and recv on loopback (AF_INET6)",
	"[udp][uring]") {
	auto fx = require_ring_fixture();
	int const probe = ::socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
	if (probe < 0) {
		WARN("AF_INET6 not available — skipping");
		return;
	}
	::close(probe);
	auto recv_sock = UdpSocket::ephemeral(fx->task_ring, AF_INET6);
	auto send_sock = UdpSocket::ephemeral(fx->task_ring, AF_INET6);
	sockaddr_storage ss{};
	socklen_t len = sizeof(ss);
	::getsockname(recv_sock.raw_fd(), reinterpret_cast<sockaddr *>(&ss), &len);
	std::uint16_t const recv_port = ntohs(reinterpret_cast<sockaddr_in6 const *>(&ss)->sin6_port);
	REQUIRE(recv_port > 0);
	std::array<std::uint8_t, 4> payload{0x01, 0x02, 0x03, 0x04};
	sockaddr_in6 dest{};
	dest.sin6_family = AF_INET6;
	dest.sin6_port = htons(recv_port);
	dest.sin6_addr = in6addr_loopback;
	sockaddr_storage dest_ss{};
	memcpy(&dest_ss, &dest, sizeof(dest));
	fx->run(send_sock.async_send_to_borrowed(
		std::span<std::uint8_t const>{payload.data(), payload.size()},
		dest_ss,
		sizeof(dest)));
	std::array<std::uint8_t, 256> rx_buf{};
	auto const rx = fx->run(recv_sock.async_recv_from(std::span<std::uint8_t>{rx_buf.data(), rx_buf.size()}));
	REQUIRE(rx.bytes == payload.size());
	CHECK(memcmp(rx_buf.data(), payload.data(), rx.bytes) == 0);
}
// ---------------------------------------------------------------------------
// async_recv_from negative timeout
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: async_recv_from with negative timeout throws EINVAL",
	"[udp]") {
	auto fx = require_ring_fixture();
	auto sock = UdpSocket::ephemeral(fx->task_ring, AF_INET);
	std::array<std::uint8_t, 256> rx_buf{};
	int err_code = 0;
	try {
		fx->run(
			sock.async_recv_from(std::span<std::uint8_t>{rx_buf.data(), rx_buf.size()}, std::chrono::milliseconds{-1}));
	} catch (IoError const &e) { err_code = e.code().value(); }
	CHECK(err_code == EINVAL);
}
