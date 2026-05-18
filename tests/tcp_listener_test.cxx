// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.socket_io;
namespace {

conflux::uring::Ring make_ring() {
	auto r = conflux::uring::Ring::init(32, {});
	REQUIRE(r);
	return move(*r);
}
// Blocking IPv4 connect to loopback:port. Returns connected fd. Caller owns it.
int connect_v4(
	std::uint16_t port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	return fd;
}
// Blocking IPv6 connect to ::1:port. Returns connected fd. Caller owns it.
int connect_v6(
	std::uint16_t port) {
	int const fd = ::socket(AF_INET6, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in6 addr{};
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(port);
	addr.sin6_addr = in6addr_loopback;
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	return fd;
}

} // namespace
TEST_CASE(
	"tcplisten.port0: ephemeral port assigned",
	"[tcp_listener]") {
	TcpListener const l{TcpListenerOptions{.bind = TcpBindAddress::loopback_v4}};
	CHECK(l.port() > 0);
	CHECK(l.raw_fd() >= 0);
}
TEST_CASE(
	"tcplisten.loopback_v4: getsockname reports 127.0.0.1",
	"[tcp_listener]") {
	TcpListener const l{TcpListenerOptions{.bind = TcpBindAddress::loopback_v4}};
	sockaddr_storage ss{};
	socklen_t sslen = sizeof(ss);
	REQUIRE(::getsockname(l.raw_fd(), reinterpret_cast<sockaddr *>(&ss), &sslen) == 0);
	REQUIRE(ss.ss_family == AF_INET);
	auto const &a4 = reinterpret_cast<sockaddr_in const &>(ss);
	CHECK(ntohl(a4.sin_addr.s_addr) == INADDR_LOOPBACK);
}
TEST_CASE(
	"tcplisten.loopback_v6: getsockname reports ::1",
	"[tcp_listener]") {
	TcpListener const l{TcpListenerOptions{.bind = TcpBindAddress::loopback_v6}};
	sockaddr_storage ss{};
	socklen_t sslen = sizeof(ss);
	REQUIRE(::getsockname(l.raw_fd(), reinterpret_cast<sockaddr *>(&ss), &sslen) == 0);
	REQUIRE(ss.ss_family == AF_INET6);
	auto const &a6 = reinterpret_cast<sockaddr_in6 const &>(ss);
	CHECK(std::memcmp(&a6.sin6_addr, &in6addr_loopback, sizeof(in6addr_loopback)) == 0);
}
TEST_CASE(
	"tcplisten.arm_v6: loopback_v6 client connect produces accept CQE",
	"[tcp_listener]") {
	auto ring = make_ring();
	SocketRawRing srr{ring.ref()};
	auto const caps = conflux::uring::detect_caps(ring);
	TcpListener l{TcpListenerOptions{.bind = TcpBindAddress::loopback_v6}};
	sockaddr_storage peer{};
	socklen_t peerlen = sizeof(peer);
	constexpr std::uint64_t UD = 0xACC3;
	REQUIRE(l.arm_accept_multishot_borrowed(srr, reinterpret_cast<sockaddr *>(&peer), &peerlen, UD, caps));
	ring.submit();
	int const client = connect_v6(l.port());
	io_uring_cqe *cqe = nullptr;
	REQUIRE(ring.wait_cqe(&cqe) == 0);
	REQUIRE(cqe);
	CHECK(cqe->user_data == UD);
	CHECK(cqe->res >= 0);
	::close(cqe->res);
	ring.cqe_seen(cqe);
	::close(client);
}
TEST_CASE(
	"tcplisten.reuse_port: second listener on same port binds",
	"[tcp_listener]") {
	TcpListener const l1{
		TcpListenerOptions{.bind = TcpBindAddress::loopback_v4, .reuse_port = true}
    };
	REQUIRE(l1.port() > 0);
	// Only assert the second bind succeeds — load-balancing is scheduler-dependent.
	TcpListener const l2{
		TcpListenerOptions{.port = l1.port(), .bind = TcpBindAddress::loopback_v4, .reuse_port = true}
    };
	CHECK(l2.port() == l1.port());
}
TEST_CASE(
	"tcplisten.arm: client connect produces accept CQE",
	"[tcp_listener]") {
	auto ring = make_ring();
	SocketRawRing srr{ring.ref()};
	auto const caps = conflux::uring::detect_caps(ring);
	TcpListener l{TcpListenerOptions{.bind = TcpBindAddress::loopback_v4}};
	sockaddr_storage peer{};
	socklen_t peerlen = sizeof(peer);
	constexpr std::uint64_t UD = 0xACC1;
	REQUIRE(l.arm_accept_multishot_borrowed(srr, reinterpret_cast<sockaddr *>(&peer), &peerlen, UD, caps));
	ring.submit();
	int const client = connect_v4(l.port());
	io_uring_cqe *cqe = nullptr;
	REQUIRE(ring.wait_cqe(&cqe) == 0);
	REQUIRE(cqe);
	CHECK(cqe->user_data == UD);
	CHECK(cqe->res >= 0);
	::close(cqe->res);
	ring.cqe_seen(cqe);
	::close(client);
}
TEST_CASE(
	"tcplisten.accept_flags: SOCK_NONBLOCK and SOCK_CLOEXEC propagate to accepted fd",
	"[tcp_listener]") {
	auto ring = make_ring();
	SocketRawRing srr{ring.ref()};
	auto const caps = conflux::uring::detect_caps(ring);
	TcpListener l{TcpListenerOptions{.bind = TcpBindAddress::loopback_v4}};
	sockaddr_storage peer{};
	socklen_t peerlen = sizeof(peer);
	constexpr std::uint64_t UD = 0xACC4;
	REQUIRE(l.arm_accept_multishot_borrowed(srr, reinterpret_cast<sockaddr *>(&peer), &peerlen, UD, caps));
	ring.submit();
	int const client = connect_v4(l.port());
	io_uring_cqe *cqe = nullptr;
	REQUIRE(ring.wait_cqe(&cqe) == 0);
	REQUIRE(cqe);
	REQUIRE(cqe->user_data == UD);
	REQUIRE(cqe->res >= 0);
	int const accepted = cqe->res;
	ring.cqe_seen(cqe);
	CHECK((::fcntl(accepted, F_GETFL) & O_NONBLOCK) != 0);
	CHECK((::fcntl(accepted, F_GETFD) & FD_CLOEXEC) != 0);
	::close(accepted);
	::close(client);
}
TEST_CASE(
	"tcplisten.rearm: cancel forces terminal CQE, second client accepted",
	"[tcp_listener]") {
	auto ring = make_ring();
	SocketRawRing srr{ring.ref()};
	auto const caps = conflux::uring::detect_caps(ring);
	TcpListener l{TcpListenerOptions{.bind = TcpBindAddress::loopback_v4}};
	sockaddr_storage peer{};
	socklen_t peerlen = sizeof(peer);
	constexpr std::uint64_t UD1 = 0xACC1;
	constexpr std::uint64_t UD2 = 0xACC2;
	constexpr std::uint64_t UD_CANCEL = 0xCA11;
	// arm + connect client 1
	REQUIRE(l.arm_accept_multishot_borrowed(srr, reinterpret_cast<sockaddr *>(&peer), &peerlen, UD1, caps));
	ring.submit();
	int const c1 = connect_v4(l.port());
	io_uring_cqe *cqe = nullptr;
	REQUIRE(ring.wait_cqe(&cqe) == 0);
	REQUIRE(cqe);
	REQUIRE(cqe->user_data == UD1);
	REQUIRE(cqe->res >= 0);
	::close(cqe->res);
	ring.cqe_seen(cqe);
	// force terminal by cancelling the multishot
	REQUIRE(submit_cancel_by_ud(srr, UD1, UD_CANCEL));
	ring.submit();
	// drain until terminal accept CQE (no MORE, user_data==UD1)
	bool terminal = false;
	while (!terminal) {
		REQUIRE(ring.wait_cqe(&cqe) == 0);
		REQUIRE(cqe);
		if (cqe->user_data == UD1 && ((cqe->flags & IORING_CQE_F_MORE) == 0u)) {
			terminal = true;
		} else if (cqe->user_data == UD1 && cqe->res >= 0) {
			::close(cqe->res); // accepted fd before cancel landed
		} else if (cqe->user_data == UD_CANCEL) {
			CHECK(cqe->res == 0);
		}
		ring.cqe_seen(cqe);
	}
	::close(c1);
	// drain any stray UD_CANCEL that may arrive after the terminal accept CQE
	{
		io_uring_cqe *stray = nullptr;
		while (ring.peek_cqe(&stray) == 0 && stray) {
			if (stray->user_data == UD_CANCEL) {
				CHECK(stray->res == 0);
			}
			ring.cqe_seen(stray);
			stray = nullptr;
		}
	}
	// rearm + connect client 2
	REQUIRE(l.rearm_accept_multishot_borrowed(srr, reinterpret_cast<sockaddr *>(&peer), &peerlen, UD2, caps));
	ring.submit();
	int const c2 = connect_v4(l.port());
	REQUIRE(ring.wait_cqe(&cqe) == 0);
	REQUIRE(cqe);
	CHECK(cqe->user_data == UD2);
	CHECK(cqe->res >= 0);
	::close(cqe->res);
	ring.cqe_seen(cqe);
	::close(c2);
}
