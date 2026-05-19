// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.tests.assert_probe_support;
import conflux.types;
import conflux.uring;
import conflux.socket_io;

// Path injected by CMake for the death-test probe binary.
#ifndef ASSERT_PROBE_BIN
	#error "ASSERT_PROBE_BIN must be defined by CMake"
#endif
namespace {


std::uint32_t recv_flags_for(
	std::uint16_t buf_id) noexcept {
	return IORING_CQE_F_BUFFER | (static_cast<std::uint32_t>(buf_id) << IORING_CQE_BUFFER_SHIFT);
}
std::uint32_t head_flags(
	BufferRing &ring) noexcept {
	return recv_flags_for(ring.ring_id_at(ring.debug_head_pos()));
}
struct Rig {
	conflux::uring::Ring uring;
	BufferRing ring;
	Rig(std::uint32_t count,std::size_t buf_size,std::uint16_t gid=0,BufferRingMode mode=BufferRingMode::classic_one_cqe_per_buffer)
:uring{[]{
				  auto r = conflux::uring::Ring::init(32, {});
				  REQUIRE(r);
				  return std::move(*r);
}()},
ring{uring.ref(),BufferRingOptions{.count=count,.buf_size=buf_size,.group_id=gid,.huge_pages=false,.mode=mode},conflux::uring::detect_caps(uring.ref())}{}
};

} // namespace
// Test 1: classic path advances head_pos by 1 per CQE; slice ID matches ring_order
TEST_CASE(
	"recv_bundle.classic: head advances by 1 per CQE",
	"[recv_bundle]") {
	Rig rig{8, 64};
	for (std::uint32_t i = 0; i < 8; ++i) {
		std::uint16_t const expected_id = rig.ring.ring_id_at(i);
		std::uint32_t const f = head_flags(rig.ring);
		auto slices = buffer_slices_from_cqe(rig.ring, 64, f, false);
		REQUIRE(slices.valid());
		REQUIRE(rig.ring.debug_head_pos() == i + 1);
		auto it = slices.begin();
		CHECK((*it).id == expected_id);
		slices.recycle_all();
	}
}
TEST_CASE(
	"recv_payload.classic: RAII recycles provided buffers",
	"[recv_payload]") {
	Rig rig{4, 64};
	for (std::uint32_t i = 0; i < 5; ++i) {
		auto payload = try_recv_payload_from_cqe(rig.ring, 8, head_flags(rig.ring), false);
		REQUIRE(payload);
		CHECK(payload->storage() == RecvPayloadStorage::provided_buffer_ring);
		CHECK(payload->pinning() == RecvPayloadPinning::kernel_buffer_ring_slot);
		CHECK_FALSE(payload->incremental());
		CHECK_FALSE(payload->multi_buffer());
		CHECK(payload->chunk_count() == 1u);
		CHECK(payload->total_size() == 8u);
	}
}
TEST_CASE(
	"recv_payload.bundle: exposes multi-buffer ownership",
	"[recv_payload]") {
	Rig rig{4, 64, 0, BufferRingMode::recv_bundle};
	auto payload = try_recv_payload_from_cqe(rig.ring, 2 * 64 + 7, head_flags(rig.ring), true);
	REQUIRE(payload);
	CHECK(payload->storage() == RecvPayloadStorage::provided_buffer_ring);
	CHECK(payload->pinning() == RecvPayloadPinning::kernel_buffer_ring_slot);
	CHECK_FALSE(payload->incremental());
	CHECK(payload->multi_buffer());
	CHECK(payload->chunk_count() == 3u);
	CHECK(payload->total_size() == 2 * 64 + 7u);
	std::size_t seen{};
	for (auto const &chunk: *payload) {
		seen += chunk.bytes.size();
	}
	CHECK(seen == 2 * 64 + 7u);
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
	std::uint32_t idx = 0;
	std::size_t total = 0;
	for (auto const &s: slices) {
		if (idx < 2) {
			CHECK(s.bytes.size() == 64u);
		} else {
			CHECK(s.bytes.size() == 37u);
		}
		total += s.bytes.size();
		++idx;
	}
	REQUIRE(total == static_cast<std::size_t>(res));
	slices.recycle_all();
}
// Test 5: bundle spanning the ring boundary (position count-1 into position 0)
TEST_CASE(
	"recv_bundle.bundle: ring wraparound",
	"[recv_bundle]") {
	Rig rig{4, 64};

	// Drain and recycle positions 0,1,2 so the next two-buffer bundle starts
	// at logical position 3 and wraps to logical position 4 / ring index 0.
	for (std::uint32_t i = 0; i < 3; ++i) {
		auto s = buffer_slices_from_cqe(rig.ring, 64, head_flags(rig.ring), false);
		REQUIRE(s.valid());
		s.recycle_all();
	}

	std::uint16_t const id_at3 = rig.ring.ring_id_at(3);
	std::uint16_t const id_at4 = rig.ring.ring_id_at(4);
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
	std::uint16_t const expected_id0 = rig.ring.ring_id_at(0); // ==0
	std::uint16_t const expected_id1 = rig.ring.ring_id_at(1); // ==1
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
	std::uint16_t const late_id = rig.ring.ring_id_at(2);
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


TEST_CASE(
	"recv_bundle.discard: selected zero-byte CQE advances window",
	"[recv_bundle]") {
	Rig rig{4, 64, 0, BufferRingMode::recv_bundle};

	REQUIRE(rig.ring.recycle_selected_buffer(0));
	CHECK(rig.ring.debug_head_pos() == 1u);

	for (std::uint16_t id = 1; id < 4; ++id) {
		auto slices = buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(id), true);
		REQUIRE(slices.valid());
		CHECK((*slices.begin()).id == id);
		slices.recycle_all();
	}

	auto recycled = buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(0), true);
	REQUIRE(recycled.valid());
	CHECK((*recycled.begin()).id == 0u);
	recycled.recycle_all();
}

// Test 9: discard via recycle_all() advances head by cnt (not 1)
TEST_CASE(
	"recv_bundle.discard: bundle head advances by cnt",
	"[recv_bundle]") {
	Rig rig{8, 64};
	int const res = 2 * 64 + 10; // 138 bytes → cnt=3
	std::uint32_t const before = rig.ring.debug_head_pos();
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
	std::uint32_t const h0 = rig.ring.debug_head_pos();
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
	std::uint32_t const before = rig.ring.debug_head_pos();
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
	std::uint32_t rc_flags = head_flags(rig.ring);
	bool scope_exit_ran = false;
	try {
		auto slices = buffer_slices_from_cqe(rig.ring, 2 * 64, rc_flags, true);
		REQUIRE(slices.count() == 2u);
		REQUIRE(rig.ring.debug_head_pos() == 2u);
		// Guard owns both recycle_all() and rc_flags=0 — mirrors append_recv_buf_to.
		conflux::tests::ScopeExit guard{[&]() noexcept {
			slices.recycle_all();
			rc_flags = 0;
			scope_exit_ran = true;
		}};
		throw std::runtime_error{"simulated append failure"};
	} catch (std::runtime_error const &) {}
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
	REQUIRE(conflux::tests::run_assert_probe(ASSERT_PROBE_BIN, "desync") == 42);
#endif
}
// Test 12: assert fires when res<0 but IORING_CQE_F_BUFFER is set (kernel invariant).
TEST_CASE(
	"recv_bundle.assert: negative res with buffer flag detected",
	"[recv_bundle][death]") {
#ifdef NDEBUG
	SKIP("assert inactive in release build");
#else
	REQUIRE(conflux::tests::run_assert_probe(ASSERT_PROBE_BIN, "neg_res_buf_flag") == 42);
#endif
}
