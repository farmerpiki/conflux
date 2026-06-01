// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.types;
import conflux.uring;

using namespace conflux::uring;
TEST_CASE(
	"IoUringCaps.detect_caps: feature bits match raw ring",
	"[caps]") {
	auto r = Ring::init(32, {});
	REQUIRE(r);
	Ring &ring = *r;
	auto caps = detect_caps(ring.ref());
	CHECK(caps.feat_nodrop == ring.has_feature(IORING_FEAT_NODROP));
	CHECK(caps.feat_recvsend_bundle == ring.has_feature(IORING_FEAT_RECVSEND_BUNDLE));
	CHECK(caps.feat_submit_stable == ring.has_feature(IORING_FEAT_SUBMIT_STABLE));
	CHECK(caps.path_lifetime_stable == (ring.has_feature(IORING_FEAT_SUBMIT_STABLE) && !ring.is_sqpoll()));
	CHECK(caps.recvsend_bundle == caps.feat_recvsend_bundle);
}
TEST_CASE(
	"IoUringCaps.detect_caps: probe fields coherent with raw probe",
	"[caps]") {
	auto r = Ring::init(32, {});
	REQUIRE(r);
	Ring &ring = *r;
	auto const caps = detect_caps(ring);
	(void)caps;
	io_uring_probe *probe = io_uring_get_probe_ring(ring.raw());
	if (!probe) {
		SKIP("kernel/liburing probe unavailable");
	}
	CHECK(caps.op_socket == (io_uring_opcode_supported(probe, IORING_OP_SOCKET) != 0));
	CHECK(caps.accept_direct_supported == caps.op_socket);
	CHECK(caps.op_uring_cmd == (io_uring_opcode_supported(probe, IORING_OP_URING_CMD) != 0));
	CHECK(caps.cmd_sock_setsockopt == caps.op_uring_cmd);
	CHECK(caps.send_zc == (io_uring_opcode_supported(probe, IORING_OP_SEND_ZC) != 0));
	CHECK(caps.recv_zc == (io_uring_opcode_supported(probe, IORING_OP_RECV_ZC) != 0));
	io_uring_free_probe(probe);
}
TEST_CASE(
	"IoUringCaps.caps_to_log_string: empty on default-constructed",
	"[caps]") {
	CHECK(caps_to_log_string(IoUringCaps{}).empty());
}
TEST_CASE(
	"IoUringCaps.caps_to_log_string: lists set caps by name",
	"[caps]") {
	IoUringCaps c{};
	c.feat_nodrop = true;
	c.recvsend_bundle = true;
	auto s = caps_to_log_string(c);
	CHECK(s.find("feat_nodrop") != std::string::npos);
	CHECK(s.find("recvsend_bundle") != std::string::npos);
	CHECK(s.find("feat_submit_stable") == std::string::npos);
}
TEST_CASE(
	"IoUringCaps.recv_poll_first: false on default-constructed",
	"[caps]") {
	IoUringCaps c{};
	CHECK(!c.recv_poll_first);
	CHECK(caps_to_log_string(c).find("recv_poll_first") == std::string::npos);
}
TEST_CASE(
	"IoUringCaps.recv_poll_first: appears in log string when set",
	"[caps]") {
	IoUringCaps c{};
	c.recv_poll_first = true;
	auto s = caps_to_log_string(c);
	CHECK(s.find("recv_poll_first") != std::string::npos);
}
TEST_CASE(
	"IoUringCaps.detect_caps: recv_poll_first is true",
	"[caps]") {
	auto r = Ring::init(32, {});
	REQUIRE(r);
	auto const caps = detect_caps(r->ref());
	CHECK(caps.recv_poll_first);
}

TEST_CASE(
	"runtime.detect_capabilities: report is coherent",
	"[caps]") {
	auto caps = conflux::runtime::detect_capabilities();
	REQUIRE(caps.has_value());
	CHECK(caps->io_uring);
	auto report = conflux::runtime::capability_report(*caps);
	CHECK(report.find("io_uring") != std::string::npos);
}
