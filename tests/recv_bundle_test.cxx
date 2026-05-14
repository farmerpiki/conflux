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
[[maybe_unused]] int run_probe(
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
	Rig(u32 count,SZ buf_size,u16 gid=0,BufferRingMode mode=BufferRingMode::classic_one_cqe_per_buffer)
:uring{[]{
				  auto r = conflux::uring::Ring::init(32, {});
				  REQUIRE(r);
				  return move(*r);
}()},
ring{uring.ref(),BufferRingOptions{.count=count,.buf_size=buf_size,.group_id=gid,.huge_pages=false,.mode=mode},conflux::uring::detect_caps(uring.ref())}{}
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

	// Drain and recycle positions 0,1,2 so the next two-buffer bundle starts
	// at logical position 3 and wraps to logical position 4 / ring index 0.
	for (u32 i = 0; i < 3; ++i) {
		auto s = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
		REQUIRE(s.valid());
		s.recycle_all();
	}

	u16 const id_at3 = rig.ring.ring_id_at(3);
	u16 const id_at4 = rig.ring.ring_id_at(4);
	REQUIRE(id_at3 == 3u);
	REQUIRE(id_at4 == 0u);

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
// Test 8: out-of-order CQEs — exact failure mode from parallel multishot recv.
TEST_CASE(
	"recv_bundle.bundle: out-of-order CQE decode",
	"[recv_bundle]") {
	Rig rig{4, 64};

	// Decode a later bundled CQE first. The bytes must still be read from
	// positions 2,3, but the software head cannot pass the unresolved gap 0,1.
	u16 const late_id = rig.ring.ring_id_at(2);
	auto late = buffer_slices_from_cqe(rig.ring, 2 * 64, recv_flags_for(late_id), true);
	REQUIRE(late.valid());
	REQUIRE(late.count() == 2u);
	CHECK(rig.ring.debug_head_pos() == 0u);
	auto lit = late.begin();
	CHECK((*lit).id == 2u);
	++lit;
	CHECK((*lit).id == 3u);

	// Recycling the later CQE must be delayed; otherwise it would overwrite
	// ring positions still needed by the earlier CQEs.
	late.recycle_all();
	CHECK(rig.ring.ring_id_at(0) == 0u);
	CHECK(rig.ring.ring_id_at(1) == 1u);

	auto first = buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(0), false);
	REQUIRE(first.valid());
	CHECK(rig.ring.debug_head_pos() == 1u);
	first.recycle_all();

	auto second = buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(1), false);
	REQUIRE(second.valid());
	CHECK(rig.ring.debug_head_pos() == 4u);
	second.recycle_all();

	// All four original buffers have now been returned in ring order.
	auto recycled = buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(0), false);
	REQUIRE(recycled.valid());
	CHECK((*recycled.begin()).id == 0u);
	recycled.recycle_all();
}

// Test 8b: real recv-bundle mode tolerates CQEs decoded out of buffer-ring head order.
TEST_CASE(
	"recv_bundle.bundle: out-of-order CQE decode in bundle mode",
	"[recv_bundle]") {
	Rig rig{8, 64, 0, BufferRingMode::recv_bundle};

	// Kernel buffer selection is ring-ordered, but CQEs from different recv ops can
	// be delivered later. Decode positions 2,3 before positions 0,1.
	auto late = buffer_slices_from_cqe(rig.ring, 2 * 64, recv_flags_for(2), true);
	REQUIRE(late.valid());
	REQUIRE(late.count() == 2u);
	auto late_it = late.begin();
	CHECK((*late_it).id == 2u);
	++late_it;
	CHECK((*late_it).id == 3u);
	late.recycle_all();

	// Recycling the late CQE must be delayed; otherwise it would overwrite
	// ring positions still needed by the earlier CQEs.
	CHECK(rig.ring.ring_id_at(0) == 0u);
	CHECK(rig.ring.ring_id_at(1) == 1u);

	auto first = buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(0), true);
	REQUIRE(first.valid());
	CHECK((*first.begin()).id == 0u);
	first.recycle_all();

	auto second = buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(1), true);
	REQUIRE(second.valid());
	CHECK((*second.begin()).id == 1u);
	second.recycle_all();
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

TEST_CASE(
	"recv_bundle.try: stale classic CQE reports bad_window",
	"[recv_bundle]") {
	Rig rig{4, 64};

	auto first = try_buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(0), false);
	REQUIRE(first);
	first->recycle_all();

	auto second = try_buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(1), false);
	REQUIRE(second);
	second->recycle_all();

	// Simulates a delayed/stale CQE observed after the userspace buffer-ring
	// window already advanced and recycled that id. Recovery paths must not hit
	// the assertion-only decoder variant.
	auto stale = try_buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(0), false);
	REQUIRE_FALSE(stale);
	CHECK(stale.error() == RecvDecodeError::bad_window);
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
