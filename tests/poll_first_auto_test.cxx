// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.socket_io;
using namespace conflux::socket_io;

static constexpr auto kSockNonempty = conflux::uring::cqe_flags::sock_nonempty;
TEST_CASE(
	"resolve_recv_arm_policy: no prior flags → default_",
	"[poll_first_auto]") {
	CHECK(resolve_recv_arm_policy(true, true, false, conflux::uring::CqeFlags{}) == RecvArmPolicy::default_);
}
TEST_CASE(
	"resolve_recv_arm_policy: cap disabled → default_",
	"[poll_first_auto]") {
	CHECK(resolve_recv_arm_policy(true, false, true, conflux::uring::CqeFlags{}) == RecvArmPolicy::default_);
}
TEST_CASE(
	"resolve_recv_arm_policy: auto disabled → default_",
	"[poll_first_auto]") {
	CHECK(resolve_recv_arm_policy(false, true, true, conflux::uring::CqeFlags{}) == RecvArmPolicy::default_);
}
TEST_CASE(
	"resolve_recv_arm_policy: SOCK_NONEMPTY set → default_",
	"[poll_first_auto]") {
	CHECK(resolve_recv_arm_policy(true, true, true, kSockNonempty) == RecvArmPolicy::default_);
}
TEST_CASE(
	"resolve_recv_arm_policy: no SOCK_NONEMPTY → poll_first",
	"[poll_first_auto]") {
	CHECK(resolve_recv_arm_policy(true, true, true, conflux::uring::CqeFlags{}) == RecvArmPolicy::poll_first);
}
