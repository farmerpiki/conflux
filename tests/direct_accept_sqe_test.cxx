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
import conflux.uring.fd;
import conflux.socket_io;

namespace {

using conflux::uring::Ring;
using namespace conflux::uring;

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
	CHECK(sqe.level == static_cast<__u32>(level));
	CHECK(sqe.optname == static_cast<__u32>(optname));
	CHECK(sqe.optlen == sizeof(int));
	CHECK(sqe.user_data == user_data);
	auto const flags = SqeFlags{sqe.flags};
	CHECK(flags.any(sqe_flags::fixed_file));
	CHECK(flags.any(sqe_flags::io_hardlink));
	if (expect_skip_cqe) {
		CHECK(flags.any(sqe_flags::cqe_skip_success));
	} else {
		CHECK_FALSE(flags.any(sqe_flags::cqe_skip_success));
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
		DirectFd::from_direct(static_cast<std::uint32_t>(direct_slot)),
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
	auto const recv_flags = SqeFlags{recv.flags};
	CHECK(recv_flags.any(sqe_flags::fixed_file));
	CHECK(recv_flags.any(sqe_flags::buffer_select));
	CHECK_FALSE(recv_flags.any(sqe_flags::io_link | sqe_flags::io_hardlink));
	unsigned expected_ioprio = conflux::uring::ioprio_flags::recvsend_poll_first.raw();
#if CONFLUX_ENABLE_RECV_BUNDLE
	expected_ioprio |= conflux::uring::ioprio_flags::recvsend_bundle.raw();
#endif
	CHECK((recv.ioprio & expected_ioprio) == expected_ioprio);
}

TEST_CASE(
	"socket_io.direct_accept_setup: accepts only direct sockets",
	"[socket_io][direct_accept][sqe]") {
	CHECK_FALSE(DirectFdLike<OsFd>);
}

TEST_CASE(
	"uring.handle: OsFd prep clears stale fixed-file flag",
	"[uring][handle][sqe]") {
	io_uring_sqe sqe{};
	conflux::uring::Sqe{&sqe}.set_flags(sqe_flags::fixed_file | sqe_flags::io_link);
	conflux::uring::Sqe sqe_view{&sqe};

	char buf[8]{};
	sqe_view.prep_read(OsFd::from_os(3), buf, sizeof(buf), 0);

	CHECK_FALSE(SqeFlags{sqe.flags}.any(sqe_flags::fixed_file));
	CHECK(SqeFlags{sqe.flags}.any(sqe_flags::io_link));
	CHECK(sqe.fd == 3);

	sqe_view.prep_read(DirectFd::from_direct(4), buf, sizeof(buf), 0);
	CHECK(SqeFlags{sqe.flags}.any(sqe_flags::fixed_file));
	CHECK(sqe.fd == 4);

	sqe_view.prep_close(DirectFd::from_direct(4));
	CHECK_FALSE(SqeFlags{sqe.flags}.any(sqe_flags::fixed_file));

	sqe_view.add_flags(sqe_flags::fixed_file);
	sqe_view.prep_cancel_fd(DirectFd::from_direct(4));
	CHECK_FALSE(SqeFlags{sqe.flags}.any(sqe_flags::fixed_file));
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
	REQUIRE(submit_direct_tcp_accept_setup_recv_to_group(raw, DirectFd::from_direct(2), target, 0x11u, 0x22u, opts));
	CHECK(ring_raw->sq.sqe_tail - tail_before == 2);
	check_cmd_setsockopt_sqe(sqe_at(ring_raw, tail_before, 0), 2, IPPROTO_TCP, TCP_NODELAY, 0x11u, true);
	CHECK(sqe_at(ring_raw, tail_before, 1).opcode == IORING_OP_RECV);
}
