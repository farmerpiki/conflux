// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.socket_io;

// Path injected by CMake for the death-test probe binary.
#ifndef ASSERT_PROBE_BIN
	#error "ASSERT_PROBE_BIN must be defined by CMake"
#endif
namespace {

// Fork the assert probe binary.
// Returns 42 if the named assert fired (probe installs SIGABRT→_exit(42)),
// 0 if the probe exited normally (NDEBUG build), negative on fork/exec failure.
int run_probe(
	char const *probe) noexcept {
	pid_t const pid = ::fork();
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		char *args[] = {const_cast<char *>(ASSERT_PROBE_BIN), const_cast<char *>(probe), nullptr};
		::execv(ASSERT_PROBE_BIN, args);
		::_exit(3); // execv failed
	}
	int status{};
	::waitpid(pid, &status, 0);
	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	return -1;
}

} // namespace
namespace {

u32 recv_flags_for(
	u16 buf_id) noexcept {
	return IORING_CQE_F_BUFFER | (static_cast<u32>(buf_id) << IORING_CQE_BUFFER_SHIFT);
}
u32 head_flags(
	BufferRing &ring) noexcept {
	return recv_flags_for(ring.ring_id_at(ring.debug_head_pos()));
}
struct Rig {
	conflux::uring::Ring uring;
	BufferRing ring;
	Rig(u32 count,SZ buf_size,u16 gid=0)
:uring{[]{
				  auto r = conflux::uring::Ring::init(32, {});
				  REQUIRE(r);
				  return move(*r);
}()},
ring{uring.ref(),BufferRingOptions{.count=count,.buf_size=buf_size,.group_id=gid,.huge_pages=false,.mode=BufferRingMode::classic_one_cqe_per_buffer},conflux::uring::detect_caps(uring.ref())}{}
};
template<typename F>
struct ScopeExit {
	F fn;
	~ScopeExit() noexcept { fn(); }
};
template<typename F>
ScopeExit(F) -> ScopeExit<F>;

} // namespace
// Test 1: classic path advances head_pos by 1 per CQE; slice ID matches ring_order
TEST_CASE(
	"recv_bundle.classic: head advances by 1 per CQE",
	"[recv_bundle]") {
	Rig rig{8, 64};
	for (u32 i = 0; i < 8; ++i) {
		u16 const expected_id = rig.ring.ring_id_at(i);
		u32 const f = head_flags(rig.ring);
		auto slices = buffer_slices_from_cqe(rig.ring, 64, f, false);
		REQUIRE(slices.valid());
		REQUIRE(rig.ring.debug_head_pos() == i + 1);
		auto it = slices.begin();
		CHECK((*it).id == expected_id);
		slices.recycle_all();
	}
}
// Test 2: bundle fits in one buffer (res < buf_size)
TEST_CASE(
	"recv_bundle.bundle: single-buffer CQE",
	"[recv_bundle]") {
	Rig rig{8, 64};
	auto slices = buffer_slices_from_cqe(rig.ring, 40, head_flags(rig.ring), true);
	REQUIRE(slices.valid());
	REQUIRE(slices.count() == 1u);
	REQUIRE(rig.ring.debug_head_pos() == 1u);
	auto s = *slices.begin();
	REQUIRE(s.bytes.size() == 40u);
	slices.recycle_all();
}
// Test 3: bundle fills exactly N full buffers (res == N * buf_size)
TEST_CASE(
	"recv_bundle.bundle: exact 3 full buffers",
	"[recv_bundle]") {
	Rig rig{8, 64};
	auto slices = buffer_slices_from_cqe(rig.ring, 3 * 64, head_flags(rig.ring), true);
	REQUIRE(slices.valid());
	REQUIRE(slices.count() == 3u);
	REQUIRE(rig.ring.debug_head_pos() == 3u);
	for (auto const &s: slices) {
		CHECK(s.bytes.size() == 64u);
	}
	slices.recycle_all();
}
// Test 4: bundle with partial last buffer (res not a multiple of buf_size)
TEST_CASE(
	"recv_bundle.bundle: partial last buffer",
	"[recv_bundle]") {
	Rig rig{8, 64};
	int const res = 2 * 64 + 37;
	auto slices = buffer_slices_from_cqe(rig.ring, res, head_flags(rig.ring), true);
	REQUIRE(slices.valid());
	REQUIRE(slices.count() == 3u);
	REQUIRE(rig.ring.debug_head_pos() == 3u);
	u32 idx = 0;
	SZ total = 0;
	for (auto const &s: slices) {
		if (idx < 2) {
			CHECK(s.bytes.size() == 64u);
		} else {
			CHECK(s.bytes.size() == 37u);
		}
		total += s.bytes.size();
		++idx;
	}
	REQUIRE(total == static_cast<SZ>(res));
	slices.recycle_all();
}
// Test 5: bundle spanning the ring boundary (position count-1 into position 0)
TEST_CASE(
	"recv_bundle.bundle: ring wraparound",
	"[recv_bundle]") {
	Rig rig{4, 64};
	// Consume positions 0,1,2 and recycle in non-sequential order so that
	// the recycled ID at ring position 0 (after wrap) != first_id+1.
	// After consume(0): recycle ID 2 to tail→pos 0; recycle ID 0 to tail→pos 1.
	auto consume_one = [&] {
		auto s = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
		REQUIRE(s.valid());
		return s;
	};
	auto s0 = consume_one(); // consumes pos 0 (ID 0), head=1
	auto s1 = consume_one(); // consumes pos 1 (ID 1), head=2
	auto s2 = consume_one(); // consumes pos 2 (ID 2), head=3
	// Recycle in order {2,0} so ring_order_[4%4=0]=2, ring_order_[5%4=1]=0.
	rig.ring.recycle(2);
	rig.ring.recycle(0);
	s1.recycle_all(); // recycles ID 1 → ring_order_[6%4=2]=1
	s2.detach(); // already manually recycled above via rig.ring.recycle
	s0.detach(); // already manually recycled above via rig.ring.recycle
	// ring_order_=[2,0,1,3], head=3.
	// ring_id_at(3)=3, ring_id_at(4)=ring_order_[4%4=0]=2 ← wrap.
	u16 const id_at3 = rig.ring.ring_id_at(3);
	u16 const id_at4 = rig.ring.ring_id_at(4); // crosses boundary
	REQUIRE(id_at3 == 3u);
	REQUIRE(id_at4 == 2u); // would be (3+1)%4=0 if using wrong algorithm
	auto slices = buffer_slices_from_cqe(rig.ring, 2 * 64, recv_flags_for(id_at3), true);
	REQUIRE(slices.valid());
	REQUIRE(slices.count() == 2u);
	REQUIRE(rig.ring.debug_head_pos() == 5u);
	auto it = slices.begin();
	CHECK((*it).id == id_at3);
	++it;
	CHECK((*it).id == id_at4);
	slices.recycle_all();
}
// Test 7: recycle_all() returns slots to the pool; ring remains operational
TEST_CASE(
	"recv_bundle.recycle_all: restores pool",
	"[recv_bundle]") {
	Rig rig{4, 64};
	// Record the IDs at positions 0,1 before consuming.
	u16 const expected_id0 = rig.ring.ring_id_at(0); // ==0
	u16 const expected_id1 = rig.ring.ring_id_at(1); // ==1
	// Consume positions 0,1 as a 2-buffer bundle.
	auto slices = buffer_slices_from_cqe(rig.ring, 2 * 64, head_flags(rig.ring), true);
	REQUIRE(slices.count() == 2u);
	REQUIRE(rig.ring.debug_head_pos() == 2u);
	// Recycle: IDs go to tail (position 4 and 5 mod 4 = positions 0,1 in ring_order_).
	slices.recycle_all();
	// Consume positions 2,3 (originals not yet consumed).
	auto s2 = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
	REQUIRE(s2.valid());
	s2.recycle_all();
	auto s3 = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
	REQUIRE(s3.valid());
	s3.recycle_all();
	// Consume positions 4,5 — the recycled IDs from positions 0,1.
	auto sr0 = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
	REQUIRE(sr0.valid());
	CHECK((*sr0.begin()).id == expected_id0);
	sr0.recycle_all();
	auto sr1 = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
	REQUIRE(sr1.valid());
	CHECK((*sr1.begin()).id == expected_id1);
	sr1.recycle_all();
}
// Test 8: non-sequential recycled IDs — exact failure mode of (buf_id+1)%count walk
TEST_CASE(
	"recv_bundle.bundle: non-sequential recycled IDs",
	"[recv_bundle]") {
	Rig rig{4, 64};
	// Consume all 4 original buffers (IDs 0,1,2,3) without recycling.
	// RecvSlices has no auto-recycle destructor; they are intentionally leaked here.
	for (u32 i = 0; i < 4; ++i) {
		auto s = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
		REQUIRE(s.valid());
	}
	REQUIRE(rig.ring.debug_head_pos() == 4u);
	// Recycle in order {3,1,2}: positions 0,1,2 of ring_order_ get {3,1,2}.
	rig.ring.recycle(3);
	rig.ring.recycle(1);
	rig.ring.recycle(2);
	// Verify placement.
	CHECK(rig.ring.ring_id_at(4) == 3u); // ring_order_[4%4=0]=3
	CHECK(rig.ring.ring_id_at(5) == 1u); // ring_order_[5%4=1]=1
	CHECK(rig.ring.ring_id_at(6) == 2u); // ring_order_[6%4=2]=2
	// 3-buffer bundle starting at ID 3.
	auto slices = buffer_slices_from_cqe(rig.ring, 3 * 64, recv_flags_for(3), true);
	REQUIRE(slices.valid());
	REQUIRE(slices.count() == 3u);
	REQUIRE(rig.ring.debug_head_pos() == 7u);
	auto it = slices.begin();
	// Correct ring_order traversal: {3,1,2}.
	// Wrong (buf_id+1)%count walk: {3,0,1}.
	CHECK((*it).id == 3u);
	++it;
	CHECK((*it).id == 1u);
	++it; // wrong algo yields 0
	CHECK((*it).id == 2u); // wrong algo yields 1
	slices.recycle_all();
}
// Test 9: discard via recycle_all() advances head by cnt (not 1)
TEST_CASE(
	"recv_bundle.discard: bundle head advances by cnt",
	"[recv_bundle]") {
	Rig rig{8, 64};
	int const res = 2 * 64 + 10; // 138 bytes → cnt=3
	u32 const before = rig.ring.debug_head_pos();
	auto slices = buffer_slices_from_cqe(rig.ring, res, head_flags(rig.ring), true);
	REQUIRE(slices.count() == 3u);
	REQUIRE(rig.ring.debug_head_pos() == before + 3u);
	slices.recycle_all();
	// consume() already ran; head stays at before+3.
	REQUIRE(rig.ring.debug_head_pos() == before + 3u);
}
// Test 10: non-recv CQEs (no IORING_CQE_F_BUFFER) do not advance head_pos
TEST_CASE(
	"recv_bundle.mixed: non-recv CQEs leave head unchanged",
	"[recv_bundle]") {
	Rig rig{8, 64};
	u32 const h0 = rig.ring.debug_head_pos();
	auto recv = [&] {
		auto s = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
		REQUIRE(s.valid());
		s.recycle_all();
	};
	auto non_recv = [&] {
		// res=0 and no buffer flag — typical for send-complete or timer CQE.
		auto s = buffer_slices_from_cqe(rig.ring, 0, 0u, false);
		CHECK(!s.valid());
	};
	recv();
	CHECK(rig.ring.debug_head_pos() == h0 + 1u); // recv CQE 1
	non_recv();
	CHECK(rig.ring.debug_head_pos() == h0 + 1u); // send-complete
	recv();
	CHECK(rig.ring.debug_head_pos() == h0 + 2u); // recv CQE 2
	non_recv();
	CHECK(rig.ring.debug_head_pos() == h0 + 2u); // timer
}
// Test 11: res>0 with no IORING_CQE_F_BUFFER → empty return, no ring mutation
TEST_CASE(
	"recv_bundle.no_buffer: positive res without buffer flag",
	"[recv_bundle]") {
	Rig rig{8, 64};
	u32 const before = rig.ring.debug_head_pos();
	auto slices = buffer_slices_from_cqe(rig.ring, 100, 0u, false);
	CHECK(!slices.valid());
	CHECK(slices.count() == 0u);
	CHECK(rig.ring.debug_head_pos() == before);
}
// Test 13: ScopeExit recycles and clears flags even when dst.append() throws
TEST_CASE(
	"recv_bundle.scope_exit: recycles on exception",
	"[recv_bundle]") {
	Rig rig{4, 64};
	u32 rc_flags = head_flags(rig.ring);
	bool scope_exit_ran = false;
	try {
		auto slices = buffer_slices_from_cqe(rig.ring, 2 * 64, rc_flags, true);
		REQUIRE(slices.count() == 2u);
		REQUIRE(rig.ring.debug_head_pos() == 2u);
		// Guard owns both recycle_all() and rc_flags=0 — mirrors append_recv_buf_to.
		ScopeExit guard{[&]() noexcept {
			slices.recycle_all();
			rc_flags = 0;
			scope_exit_ran = true;
		}};
		throw RE{"simulated append failure"};
	} catch (RE const &) {}
	// Stack unwinding ran the guard before catch.
	CHECK(scope_exit_ran);
	CHECK(rc_flags == 0u);
	// head advanced (consume happened before throw); ring still operational.
	CHECK(rig.ring.debug_head_pos() == 2u);
	// Recycled positions 0,1 back to tail → can consume 2,3 then the recycled pair.
	auto s2 = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
	REQUIRE(s2.valid());
	s2.recycle_all();
	auto s3 = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
	REQUIRE(s3.valid());
	s3.recycle_all();
	auto sr = buffer_slices_from_cqe(rig.ring, 2 * 64, head_flags(rig.ring), true);
	REQUIRE(sr.valid());
	REQUIRE(sr.count() == 2u);
	sr.recycle_all();
}
// Test 6: assert fires when CQE buf_id does not match ring_order_[head_pos].
// Requires debug build; LD_PRELOAD interceptor converts abort→exit(42).
TEST_CASE(
	"recv_bundle.assert: ID mismatch detected",
	"[recv_bundle][death]") {
#ifdef NDEBUG
	SKIP("assert inactive in release build");
#else
	// ring_id_at(head_pos=0)==0, but probe passes buf_id=5 → mismatch.
	REQUIRE(run_probe("desync") == 42);
#endif
}
// Test 12: assert fires when res<0 but IORING_CQE_F_BUFFER is set (kernel invariant).
TEST_CASE(
	"recv_bundle.assert: negative res with buffer flag detected",
	"[recv_bundle][death]") {
#ifdef NDEBUG
	SKIP("assert inactive in release build");
#else
	REQUIRE(run_probe("neg_res_buf_flag") == 42);
#endif
}
