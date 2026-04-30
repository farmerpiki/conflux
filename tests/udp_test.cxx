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
import conflux.file_io;
import conflux.net.udp;

using namespace std;
using namespace conflux::net::udp;

namespace {

constexpr uint64_t pack_ud(
	uint32_t slot,
	uint32_t gen) noexcept {
	return (static_cast<uint64_t>(gen) << 32U) | slot;
}

struct RingFixture {
	::io_uring ring{};
	CompletionTable completions{};
	FileReader reader;
	bool ring_ok{false};

	RingFixture()
		: reader{&ring, &completions, [](uint32_t slot, uint32_t gen) noexcept { return pack_ud(slot, gen); }} {}

	static unique_ptr<RingFixture> make(
		unsigned entries = 64) {
		auto fx = make_unique<RingFixture>();
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
		chrono::milliseconds budget = chrono::seconds{5}) {
		return block_on(reader, std::move(task), budget);
	}
};

unique_ptr<RingFixture> require_ring_fixture(
	unsigned entries = 64) {
	auto fx = RingFixture::make(entries);
	INFO("conflux requires a host that permits io_uring_queue_init");
	REQUIRE(fx != nullptr);
	return fx;
}

::sockaddr_in v4_loopback(
	uint16_t port) noexcept {
	::sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return sa;
}

} // namespace

// ---------------------------------------------------------------------------
// UdpSocket — RAII / open_ephemeral / local_port
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: open_ephemeral binds to non-zero local port",
	"[udp]") {
	auto sock = UdpSocket::open_ephemeral(AF_INET);
	CHECK(sock.is_open());
	CHECK(sock.family() == AF_INET);
	CHECK(sock.raw_fd() >= 0);
	CHECK(sock.local_port() > 0);
}

TEST_CASE(
	"udp: open_ephemeral works for AF_INET6",
	"[udp]") {
	auto sock = UdpSocket::open_ephemeral(AF_INET6);
	CHECK(sock.is_open());
	CHECK(sock.family() == AF_INET6);
	CHECK(sock.local_port() > 0);
}

TEST_CASE(
	"udp: UdpSocket move-only — source becomes empty",
	"[udp]") {
	auto src = UdpSocket::open_ephemeral(AF_INET);
	int const fd = src.raw_fd();
	REQUIRE(fd >= 0);
	auto dst = std::move(src);
	CHECK_FALSE(src.is_open());
	CHECK(dst.raw_fd() == fd);
}

// ---------------------------------------------------------------------------
// sendto + recvfrom — loopback round-trip
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: send and recv on loopback (AF_INET)",
	"[udp][uring]") {
	auto fx = require_ring_fixture();

	auto recv_sock = UdpSocket::open_ephemeral(AF_INET);
	auto send_sock = UdpSocket::open_ephemeral(AF_INET);

	auto const recv_port = recv_sock.local_port();
	REQUIRE(recv_port > 0);

	std::array<uint8_t, 5> payload{'h', 'e', 'l', 'l', 'o'};
	auto dest = v4_loopback(recv_port);

	// Send first; kernel buffers the datagram.
	auto const bytes_sent = fx->run(sendto(
		fx->reader,
		send_sock,
		span<uint8_t const>{payload.data(), payload.size()},
		reinterpret_cast<::sockaddr const *>(&dest),
		sizeof(dest)));
	CHECK(bytes_sent == payload.size());

	// Recv now — packet is already in the kernel buffer.
	std::array<uint8_t, 256> rx_buf{};
	auto const rx = fx->run(recvfrom(fx->reader, recv_sock, span<uint8_t>{rx_buf.data(), rx_buf.size()}));

	REQUIRE(rx.bytes == payload.size());
	CHECK(memcmp(rx_buf.data(), payload.data(), rx.bytes) == 0);
	CHECK(rx.from_len >= sizeof(::sockaddr_in));
	auto const &from = *reinterpret_cast<::sockaddr_in const *>(&rx.from);
	CHECK(from.sin_family == AF_INET);
	CHECK(from.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
	CHECK(ntohs(from.sin_port) == send_sock.local_port());
}

// ---------------------------------------------------------------------------
// recvfrom_with_timeout — fires when no data arrives
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: recvfrom_with_timeout fires UdpError on idle socket",
	"[udp][uring]") {
	auto fx = require_ring_fixture();

	auto sock = UdpSocket::open_ephemeral(AF_INET);

	std::array<uint8_t, 256> rx_buf{};
	int err_code = 0;
	bool got_value = false;
	try {
		fx->run(
			recvfrom_with_timeout(
				fx->reader,
				sock,
				span<uint8_t>{rx_buf.data(), rx_buf.size()},
				chrono::milliseconds{50}),
			chrono::seconds{2});
		got_value = true;
	} catch (UdpError const &ue) { err_code = ue.code().value(); } catch (...) {
	}

	CHECK_FALSE(got_value);
	CHECK(err_code == ETIMEDOUT);
}

// ---------------------------------------------------------------------------
// recvfrom_with_timeout — succeeds when data arrives in time
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: recvfrom_with_timeout receives packet that arrives before timeout",
	"[udp][uring]") {
	auto fx = require_ring_fixture();

	auto recv_sock = UdpSocket::open_ephemeral(AF_INET);
	auto send_sock = UdpSocket::open_ephemeral(AF_INET);

	auto const recv_port = recv_sock.local_port();

	std::array<uint8_t, 4> payload{0xDE, 0xAD, 0xBE, 0xEF};
	auto dest = v4_loopback(recv_port);

	// Send first; kernel buffers the datagram.
	fx->run(sendto(
		fx->reader,
		send_sock,
		span<uint8_t const>{payload.data(), payload.size()},
		reinterpret_cast<::sockaddr const *>(&dest),
		sizeof(dest)));

	std::array<uint8_t, 256> rx_buf{};
	auto const rx = fx->run(
		recvfrom_with_timeout(
			fx->reader,
			recv_sock,
			span<uint8_t>{rx_buf.data(), rx_buf.size()},
			chrono::milliseconds{2000}),
		chrono::seconds{3});

	REQUIRE(rx.bytes == payload.size());
	CHECK(memcmp(rx_buf.data(), payload.data(), rx.bytes) == 0);
}

// ---------------------------------------------------------------------------
// sendto — invalid destination is rejected synchronously
// ---------------------------------------------------------------------------

TEST_CASE(
	"udp: sendto rejects null destination",
	"[udp]") {
	auto fx = require_ring_fixture();
	auto sock = UdpSocket::open_ephemeral(AF_INET);

	std::array<uint8_t, 1> payload{0};
	int err_code = 0;
	try {
		fx->run(sendto(fx->reader, sock, span<uint8_t const>{payload.data(), payload.size()}, nullptr, 0));
	} catch (UdpError const &ue) { err_code = ue.code().value(); } catch (...) {
	}

	CHECK(err_code == EINVAL);
}
