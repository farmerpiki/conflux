// Plain TU — direct-accept SQE inspection does not need a module test unit.
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.socket_io;

namespace {

using conflux::uring::Ring;

[[nodiscard]] io_uring_sqe const &sqe_at(
	io_uring const *ring,
	unsigned tail_before,
	unsigned offset) noexcept {
	return ring->sq.sqes[(tail_before + offset) & ring->sq.ring_mask];
}
[[nodiscard]] int const *int_optval(
	io_uring_sqe const &sqe) noexcept {
	return reinterpret_cast<int const *>(static_cast<std::uintptr_t>(sqe.optval));
}
void check_cmd_setsockopt_sqe(
	io_uring_sqe const &sqe,
	int direct_slot,
	int level,
	int optname,
	std::uint64_t user_data,
	bool expect_skip_cqe) {
	CHECK(sqe.opcode == IORING_OP_URING_CMD);
	CHECK(sqe.cmd_op == static_cast<unsigned>(conflux::uring::uring_cmd_op::setsockopt));
	CHECK(sqe.fd == direct_slot);
	CHECK(sqe.level == level);
	CHECK(sqe.optname == optname);
	CHECK(sqe.optlen == sizeof(int));
	CHECK(sqe.user_data == user_data);
	CHECK((sqe.flags & IOSQE_FIXED_FILE) != 0);
	CHECK((sqe.flags & IOSQE_IO_HARDLINK) != 0);
	if (expect_skip_cqe) {
		CHECK((sqe.flags & IOSQE_CQE_SKIP_SUCCESS) != 0);
	} else {
		CHECK((sqe.flags & IOSQE_CQE_SKIP_SUCCESS) == 0);
	}
}

} // namespace

TEST_CASE(
	"socket_io.direct_accept_setup: emits TCP_NODELAY, busy-poll, then recv SQEs",
	"[socket_io][direct_accept][sqe]") {
	auto built = Ring::init(8, conflux::uring::SetupFlags{});
	REQUIRE(built.has_value());
	auto ring = std::move(*built);
	auto raw = SocketRawRing{ring.ref()};
	int const direct_slot = 7;
	int busy_poll_us = 75;
	DirectTcpAcceptSetup opts{};
	opts.tcp_nodelay_once = true;
	opts.tcp_quickack_once = true;
	opts.prefer_busy_poll_once = true;
	opts.busy_poll_us_optval = &busy_poll_us;
	opts.recv_bundle = true;
	opts.recv_arm_policy = RecvArmPolicy::poll_first;
	opts.skip_sockopt_success_cqes = true;
	DirectTcpAcceptRecvTarget target{
		.buf_group = 23,
		.buffer_mode = BufferRingMode::recv_bundle,
	};

	auto *ring_raw = ring.raw();
	unsigned const tail_before = ring_raw->sq.sqe_tail;
	REQUIRE(submit_direct_tcp_accept_setup_recv_to_group(
		raw,
		SocketHandle::from_direct(static_cast<std::uint32_t>(direct_slot)),
		target,
		0x11u,
		0x22u,
		opts));
	unsigned const tail_after = ring_raw->sq.sqe_tail;
	REQUIRE(tail_after - tail_before == 5);

	auto const &nodelay = sqe_at(ring_raw, tail_before, 0);
	check_cmd_setsockopt_sqe(nodelay, direct_slot, IPPROTO_TCP, TCP_NODELAY, 0x11u, true);
	REQUIRE(int_optval(nodelay) != nullptr);
	CHECK(*int_optval(nodelay) == 1);

	auto const &quickack = sqe_at(ring_raw, tail_before, 1);
	check_cmd_setsockopt_sqe(quickack, direct_slot, IPPROTO_TCP, TCP_QUICKACK, 0x11u, true);
	REQUIRE(int_optval(quickack) != nullptr);
	CHECK(*int_optval(quickack) == 1);

	auto const &prefer_busy_poll = sqe_at(ring_raw, tail_before, 2);
	check_cmd_setsockopt_sqe(prefer_busy_poll, direct_slot, SOL_SOCKET, SO_PREFER_BUSY_POLL, 0x11u, true);
	REQUIRE(int_optval(prefer_busy_poll) != nullptr);
	CHECK(*int_optval(prefer_busy_poll) == 1);

	auto const &busy_poll = sqe_at(ring_raw, tail_before, 3);
	check_cmd_setsockopt_sqe(busy_poll, direct_slot, SOL_SOCKET, SO_BUSY_POLL, 0x11u, true);
	CHECK(int_optval(busy_poll) == &busy_poll_us);

	auto const &recv = sqe_at(ring_raw, tail_before, 4);
	CHECK(recv.opcode == IORING_OP_RECV);
	CHECK(recv.fd == direct_slot);
	CHECK(recv.addr == 0);
	CHECK(recv.len == 0);
	CHECK(recv.buf_group == target.buf_group);
	CHECK(recv.user_data == 0x22u);
	CHECK((recv.flags & IOSQE_FIXED_FILE) != 0);
	CHECK((recv.flags & IOSQE_BUFFER_SELECT) != 0);
	CHECK((recv.flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK)) == 0);
	unsigned expected_ioprio = conflux::uring::ioprio_flags::recvsend_poll_first.raw();
#if CONFLUX_ENABLE_RECV_BUNDLE
	expected_ioprio |= conflux::uring::ioprio_flags::recvsend_bundle.raw();
#endif
	CHECK((recv.ioprio & expected_ioprio) == expected_ioprio);
}

TEST_CASE(
	"socket_io.direct_accept_setup: refuses non-direct sockets without consuming SQEs",
	"[socket_io][direct_accept][sqe]") {
	auto built = Ring::init(4, conflux::uring::SetupFlags{});
	REQUIRE(built.has_value());
	auto ring = std::move(*built);
	auto raw = SocketRawRing{ring.ref()};
	DirectTcpAcceptSetup opts{};
	opts.tcp_nodelay_once = true;
	DirectTcpAcceptRecvTarget target{.buf_group = 3};

	auto *ring_raw = ring.raw();
	unsigned const tail_before = ring_raw->sq.sqe_tail;
	CHECK_FALSE(submit_direct_tcp_accept_setup_recv_to_group(
		raw,
		SocketHandle::from_os(42),
		target,
		0x11u,
		0x22u,
		opts));
	CHECK(ring_raw->sq.sqe_tail == tail_before);
}

TEST_CASE(
	"socket_io.direct_accept_setup: non-positive busy poll does not emit SQE",
	"[socket_io][direct_accept][sqe]") {
	auto built = Ring::init(4, conflux::uring::SetupFlags{});
	REQUIRE(built.has_value());
	auto ring = std::move(*built);
	auto raw = SocketRawRing{ring.ref()};
	int busy_poll_us = 0;
	DirectTcpAcceptSetup opts{};
	opts.tcp_nodelay_once = true;
	opts.busy_poll_us_optval = &busy_poll_us;
	DirectTcpAcceptRecvTarget target{.buf_group = 3};

	auto *ring_raw = ring.raw();
	unsigned const tail_before = ring_raw->sq.sqe_tail;
	REQUIRE(submit_direct_tcp_accept_setup_recv_to_group(
		raw,
		SocketHandle::from_direct(2),
		target,
		0x11u,
		0x22u,
		opts));
	CHECK(ring_raw->sq.sqe_tail - tail_before == 2);
	check_cmd_setsockopt_sqe(sqe_at(ring_raw, tail_before, 0), 2, IPPROTO_TCP, TCP_NODELAY, 0x11u, true);
	CHECK(sqe_at(ring_raw, tail_before, 1).opcode == IORING_OP_RECV);
}
