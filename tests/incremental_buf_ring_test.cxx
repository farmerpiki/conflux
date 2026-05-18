// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.tests.assert_probe_support;
import conflux.types;
import conflux.uring;
import conflux.socket_io;

#ifndef ASSERT_PROBE_BIN
	#error "ASSERT_PROBE_BIN must be defined by CMake"
#endif
namespace {

std::uint32_t inc_flags(
	std::uint16_t buf_id,
	bool buf_more) noexcept {
	std::uint32_t f = IORING_CQE_F_BUFFER | (static_cast<std::uint32_t>(buf_id) << IORING_CQE_BUFFER_SHIFT);
	if (buf_more) {
		f |= IORING_CQE_F_BUF_MORE;
	}
	return f;
}
struct Rig {
	conflux::uring::Ring uring;
	conflux::uring::IoUringCaps caps;
	BufferRing ring;
	Rig(
		std::uint32_t count,
		std::size_t buf_size,
		std::uint16_t gid = 0)
		: uring{[] {
			auto r = conflux::uring::Ring::init(32, {});
			REQUIRE(r);
			return move(*r);
		}()}
		, caps{conflux::uring::detect_caps(uring.ref())}
		, ring{[&]() -> BufferRing {
			if (!caps.feat_pbuf_ring_inc) {
				SKIP("kernel lacks IORING_FEAT_PBUF_RING_INC");
			}
			return {
				uring.ref(),
				BufferRingOptions{
								  .count = count,
								  .buf_size = buf_size,
								  .group_id = gid,
								  .huge_pages = false,
								  .mode = BufferRingMode::incremental},
				caps
            };
		}()} {}
};

} // namespace
// ─── Original tests ───────────────────────────────────────────────────────────

// BUF_MORE CQE: head unchanged, offset=0, more()=true, bytes span correct size.
TEST_CASE(
	"incremental: BUF_MORE leaves head unchanged",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	auto slice = buffer_slice_from_incremental_cqe(rig.ring, 4, inc_flags(0, true));
	CHECK(rig.ring.debug_head_pos() == h0); // head must NOT advance on BUF_MORE
	CHECK(slice.more());
	CHECK(slice.id() == 0u);
	CHECK(slice.offset() == std::size_t{0});
	CHECK(slice.size() == std::size_t{4});
	CHECK(slice.bytes().size() == 4u);
	// No recycle yet — more still set.
	slice.recycle_if_final(); // must be a no-op
	CHECK(rig.ring.debug_head_pos() == h0);
}
// Final CQE after two BUF_MORE: offsets accumulate, head advances once at the end.
TEST_CASE(
	"recv_payload.incremental: exposes final-vs-partial ownership",
	"[recv_payload][incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	{
		auto partial = try_recv_payload_from_cqe(rig.ring, 7, inc_flags(0, true), false);
		REQUIRE(partial);
		CHECK(partial->storage() == RecvPayloadStorage::provided_buffer_ring);
		CHECK(partial->pinning() == RecvPayloadPinning::kernel_buffer_ring_slot);
		CHECK(partial->incremental());
		CHECK_FALSE(partial->multi_buffer());
		CHECK(partial->chunk_count() == 1u);
		CHECK(partial->total_size() == 7u);
		CHECK(rig.ring.debug_head_pos() == h0);
	}
	CHECK(rig.ring.debug_head_pos() == h0);
	{
		auto final = try_recv_payload_from_cqe(rig.ring, 5, inc_flags(0, false), false);
		REQUIRE(final);
		CHECK(final->incremental());
		CHECK(final->chunk_count() == 1u);
		CHECK(final->total_size() == 5u);
		CHECK(rig.ring.debug_head_pos() == h0 + 1u);
	}
}

TEST_CASE(
	"incremental: final CQE advances head by 1, offsets accumulate",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	auto s0 = buffer_slice_from_incremental_cqe(rig.ring, 4, inc_flags(0, true));
	CHECK(rig.ring.debug_head_pos() == h0);
	CHECK(s0.offset() == std::size_t{0});
	CHECK(s0.size() == std::size_t{4});
	auto s1 = buffer_slice_from_incremental_cqe(rig.ring, 8, inc_flags(0, true));
	CHECK(rig.ring.debug_head_pos() == h0);
	CHECK(s1.offset() == std::size_t{4});
	CHECK(s1.size() == std::size_t{8});
	auto s2 = buffer_slice_from_incremental_cqe(rig.ring, 20, inc_flags(0, false));
	CHECK(rig.ring.debug_head_pos() == h0 + 1u); // head advances on final CQE
	CHECK(s2.offset() == std::size_t{12});
	CHECK(s2.size() == std::size_t{20});
	CHECK(!s2.more());
	s2.recycle_if_final(); // recycles buffer back to pool
	// Next CQE for ID 0 starts at offset 0 again (new buffer acquisition).
	auto s3 = buffer_slice_from_incremental_cqe(rig.ring, 4, inc_flags(0, true));
	CHECK(s3.offset() == std::size_t{0});
	CHECK(rig.ring.debug_head_pos() == h0 + 1u); // still BUF_MORE — no advance
}
// Different buffer IDs track their offsets independently.
TEST_CASE(
	"incremental: independent per-ID offset tracking",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	// Start both IDs with BUF_MORE.
	auto s0a = buffer_slice_from_incremental_cqe(rig.ring, 10, inc_flags(0, true));
	auto s1a = buffer_slice_from_incremental_cqe(rig.ring, 20, inc_flags(1, true));
	CHECK(rig.ring.debug_head_pos() == h0);
	CHECK(s0a.offset() == std::size_t{0});
	CHECK(s1a.offset() == std::size_t{0});
	// Finalize ID 1 first.
	auto s1b = buffer_slice_from_incremental_cqe(rig.ring, 15, inc_flags(1, false));
	CHECK(s1b.offset() == std::size_t{20});
	CHECK(rig.ring.debug_head_pos() == h0 + 1u);
	s1b.recycle_if_final();
	// ID 0 continues independently.
	auto s0b = buffer_slice_from_incremental_cqe(rig.ring, 10, inc_flags(0, false));
	CHECK(s0b.offset() == std::size_t{10});
	CHECK(rig.ring.debug_head_pos() == h0 + 2u);
	s0b.recycle_if_final();
}
// recycle_if_final() on BUF_MORE slice is a no-op; slice stays valid.
TEST_CASE(
	"incremental: recycle_if_final no-op when more=true",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	auto slice = buffer_slice_from_incremental_cqe(rig.ring, 4, inc_flags(0, true));
	CHECK(slice.more());
	CHECK(slice.valid());
	slice.recycle_if_final();
	slice.recycle_if_final(); // also idempotent for BUF_MORE slices
	CHECK(rig.ring.debug_head_pos() == h0);
	CHECK(slice.valid()); // ring_ not cleared when more=true
}
// recycle_if_final() is idempotent on final slice: second call does nothing.
TEST_CASE(
	"incremental: recycle_if_final idempotent on final CQE",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	auto slice = buffer_slice_from_incremental_cqe(rig.ring, 32, inc_flags(0, false));
	CHECK(!slice.more());
	CHECK(rig.ring.debug_head_pos() == h0 + 1u);
	slice.recycle_if_final();
	CHECK(!slice.valid()); // ring_ cleared after first recycle
	slice.recycle_if_final(); // must not crash or double-recycle
}
// ScopeExit pattern: recycle_if_final() runs on exception unwind.
TEST_CASE(
	"incremental: ScopeExit recycles on exception",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t rc_flags = inc_flags(0, false);
	bool scope_ran = false;
	try {
		auto slice = buffer_slice_from_incremental_cqe(rig.ring, 32, rc_flags);
		REQUIRE(!slice.more());
		conflux::tests::ScopeExit guard{[&]() noexcept {
			slice.recycle_if_final();
			if (!slice.more()) {
				rc_flags = 0;
			}
			scope_ran = true;
		}};
		throw std::runtime_error{"simulated failure"};
	} catch (std::runtime_error const &) {}
	CHECK(scope_ran);
	CHECK(rc_flags == 0u);
	// Ring still operational: next CQE works.
	auto next = buffer_slice_from_incremental_cqe(rig.ring, 8, inc_flags(1, false));
	CHECK(next.valid());
	next.recycle_if_final();
}
// bytes() span reflects the correct offset into the slab (size at minimum).
TEST_CASE(
	"incremental: bytes() span size matches len",
	"[incremental]") {
	Rig rig{8, 64};
	auto s0 = buffer_slice_from_incremental_cqe(rig.ring, 13, inc_flags(2, true));
	CHECK(s0.bytes().size() == 13u);
	auto s1 = buffer_slice_from_incremental_cqe(rig.ring, 27, inc_flags(2, false));
	CHECK(s1.bytes().size() == 27u);
	// The two spans must be adjacent: s1.data == s0.data + s0.size
	CHECK(s1.bytes().data() == s0.bytes().data() + 13u);
	s1.recycle_if_final();
}
// Assert probe: res<0 with IORING_CQE_F_BUFFER → assert(res>0) fires.
TEST_CASE(
	"incremental.assert: negative res with buffer flag detected",
	"[incremental][death]") {
#ifdef NDEBUG
	SKIP("assert inactive in release build");
#else
	{
		auto r = conflux::uring::Ring::init(32, {});
		REQUIRE(r);
		if (!conflux::uring::detect_caps(r->ref()).feat_pbuf_ring_inc) {
			SKIP("kernel lacks IORING_FEAT_PBUF_RING_INC");
		}
	}
	REQUIRE(conflux::tests::run_assert_probe(ASSERT_PROBE_BIN, "inc_neg_res") == 42);
#endif
}
// Assert probe: res>0 but IORING_CQE_F_BUFFER absent → assert(cqe_has_buffer) fires.
TEST_CASE(
	"incremental.assert: missing buffer flag detected",
	"[incremental][death]") {
#ifdef NDEBUG
	SKIP("assert inactive in release build");
#else
	{
		auto r = conflux::uring::Ring::init(32, {});
		REQUIRE(r);
		if (!conflux::uring::detect_caps(r->ref()).feat_pbuf_ring_inc) {
			SKIP("kernel lacks IORING_FEAT_PBUF_RING_INC");
		}
	}
	REQUIRE(conflux::tests::run_assert_probe(ASSERT_PROBE_BIN, "inc_no_buf_flag") == 42);
#endif
}
// Assert probe: classic ring + incremental flags → assert(mode==incremental) fires.
TEST_CASE(
	"incremental.assert: wrong ring mode detected",
	"[incremental][death]") {
#ifdef NDEBUG
	SKIP("assert inactive in release build");
#else
	REQUIRE(conflux::tests::run_assert_probe(ASSERT_PROBE_BIN, "inc_wrong_mode") == 42);
#endif
}
// Assert probe: buf_id >= count → assert(id < ring.count()) fires.
TEST_CASE(
	"incremental.assert: out-of-range buffer id detected",
	"[incremental][death]") {
#ifdef NDEBUG
	SKIP("assert inactive in release build");
#else
	{
		auto r = conflux::uring::Ring::init(32, {});
		REQUIRE(r);
		if (!conflux::uring::detect_caps(r->ref()).feat_pbuf_ring_inc) {
			SKIP("kernel lacks IORING_FEAT_PBUF_RING_INC");
		}
	}
	REQUIRE(conflux::tests::run_assert_probe(ASSERT_PROBE_BIN, "inc_bad_id") == 42);
#endif
}
// Assert probe: res > buf_size → assert(res <= buf_size - off) fires.
TEST_CASE(
	"incremental.assert: length overflow detected",
	"[incremental][death]") {
#ifdef NDEBUG
	SKIP("assert inactive in release build");
#else
	{
		auto r = conflux::uring::Ring::init(32, {});
		REQUIRE(r);
		if (!conflux::uring::detect_caps(r->ref()).feat_pbuf_ring_inc) {
			SKIP("kernel lacks IORING_FEAT_PBUF_RING_INC");
		}
	}
	REQUIRE(conflux::tests::run_assert_probe(ASSERT_PROBE_BIN, "inc_len_overflow") == 42);
#endif
}
// ─── New tests (proposal §Tests required) ────────────────────────────────────

// T1: phase3 safety-net — cleared flags block re-decode, no double offset advance.
TEST_CASE(
	"incremental: phase3 safety-net: cleared flags → bad_cqe, offset unchanged",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	// First decode: BUF_MORE CQE, offset advances 0→4.
	auto r1 = try_buffer_slice_from_incremental_cqe(rig.ring, 4, inc_flags(0, true));
	REQUIRE(r1.has_value());
	CHECK(r1->offset() == std::size_t{0});
	CHECK(r1->size() == std::size_t{4});
	CHECK(rig.ring.debug_head_pos() == h0); // BUF_MORE: head not advanced
	// Simulate rc.flags=0 (cleared by append_recv_buf_to): phase3 re-decode blocked.
	auto r2 = try_buffer_slice_from_incremental_cqe(rig.ring, 4, 0u);
	CHECK(!r2.has_value());
	CHECK(r2.error() == RecvDecodeError::bad_cqe);
	// Offset must not have advanced a second time.
	// Verify by decoding next valid BUF_MORE: should see offset=4, not 8.
	auto r3 = try_buffer_slice_from_incremental_cqe(rig.ring, 6, inc_flags(0, true));
	REQUIRE(r3.has_value());
	CHECK(r3->offset() == std::size_t{4});
	CHECK(rig.ring.debug_head_pos() == h0);
}
// T2: Append clears rc.flags for BUF_MORE — second decode with original flags double-advances,
//     but cleared flags (0) returns bad_cqe without mutating offset.
TEST_CASE(
	"incremental: append clears rc.flags for BUF_MORE: bad_cqe on re-call with flags=0",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	auto r1 = try_buffer_slice_from_incremental_cqe(rig.ring, 10, inc_flags(0, true));
	REQUIRE(r1.has_value());
	CHECK(r1->offset() == std::size_t{0});
	// Cleared-flags re-call: decoder rejects (no BUFFER bit), offset unchanged.
	auto r2 = try_buffer_slice_from_incremental_cqe(rig.ring, 10, 0u);
	CHECK(!r2.has_value());
	CHECK(r2.error() == RecvDecodeError::bad_cqe);
	// Confirm offset still at 10 (only advanced once).
	auto r3 = try_buffer_slice_from_incremental_cqe(rig.ring, 5, inc_flags(0, false));
	REQUIRE(r3.has_value());
	CHECK(r3->offset() == std::size_t{10});
	CHECK(rig.ring.debug_head_pos() == h0 + 1u);
}
// T3: Append final — head+1 after final CQE; destructor recycles once; second
//     try_buffer_slice on same ID starts at offset 0 (new acquisition).
TEST_CASE(
	"incremental: append final: head+1, RAII destructor recycles once",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	{
		auto r = try_buffer_slice_from_incremental_cqe(rig.ring, 20, inc_flags(0, false));
		REQUIRE(r.has_value());
		CHECK(!r->more());
		CHECK(rig.ring.debug_head_pos() == h0 + 1u);
		// Destructor runs here — recycles once.
	}
	// Head must not have advanced a second time.
	CHECK(rig.ring.debug_head_pos() == h0 + 1u);
	// Next acquisition on same ID starts at offset 0.
	auto r2 = try_buffer_slice_from_incremental_cqe(rig.ring, 5, inc_flags(0, true));
	REQUIRE(r2.has_value());
	CHECK(r2->offset() == std::size_t{0});
	CHECK(rig.ring.debug_head_pos() == h0 + 1u); // BUF_MORE: still no advance
}
// T4: Close after partial — BUF_MORE CQE then reclaim_incremental_partial:
//     offset reset and buffer recycled (head+1).
TEST_CASE(
	"incremental: close after partial: reclaim_incremental_partial resets offset and recycles",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	// Partial fill: BUF_MORE CQE.
	auto r = try_buffer_slice_from_incremental_cqe(rig.ring, 12, inc_flags(0, true));
	REQUIRE(r.has_value());
	CHECK(r->offset() == std::size_t{0});
	CHECK(rig.ring.debug_head_pos() == h0);
	// Connection closed: reclaim partial buffer.
	bool const ok = rig.ring.reclaim_incremental_partial(0);
	CHECK(ok);
	CHECK(rig.ring.debug_head_pos() == h0 + 1u); // consume(1) was called
	// Next CQE on same ID starts fresh at offset 0.
	auto r2 = try_buffer_slice_from_incremental_cqe(rig.ring, 5, inc_flags(0, false));
	REQUIRE(r2.has_value());
	CHECK(r2->offset() == std::size_t{0});
}
// T5: No false recycle after final — offset is 0 after final CQE;
//     reclaim_incremental_partial must return false and not recycle.
TEST_CASE(
	"incremental: no false recycle after final: reclaim_incremental_partial returns false when offset=0",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	// Final CQE: offset reset to 0, head+1 inside decoder.
	{
		auto r = try_buffer_slice_from_incremental_cqe(rig.ring, 20, inc_flags(0, false));
		REQUIRE(r.has_value());
		// head advanced inside decoder for final CQE
		CHECK(rig.ring.debug_head_pos() == h0 + 1u);
	}
	// Now offset[0]==0. reclaim_incremental_partial must not recycle again.
	bool const ok = rig.ring.reclaim_incremental_partial(0);
	CHECK(!ok);
	CHECK(rig.ring.debug_head_pos() == h0 + 1u); // head must not advance again
}
// T6: Cap guard — fake IoUringCaps with feat_pbuf_ring_inc=false → constructor throws
//     before io_uring_setup_buf_ring is reached.
TEST_CASE(
	"incremental: BufferRing throws when feat_pbuf_ring_inc=false",
	"[incremental]") {
	auto r = conflux::uring::Ring::init(32, {});
	REQUIRE(r);
	conflux::uring::IoUringCaps fake{};
	// feat_pbuf_ring_inc left false (zero-initialized)
	auto make_bad = [&] {
		BufferRing{
			r->ref(),
			BufferRingOptions{
							  .count = 8,
							  .buf_size = 64,
							  .group_id = 7,
							  .huge_pages = false,
							  .mode = BufferRingMode::incremental},
			fake
        };
	};
	REQUIRE_THROWS_AS(make_bad(), std::runtime_error);
}
// T7: Skip when kernel lacks IORING_FEAT_PBUF_RING_INC — Rig SKIP fires, this test
//     also verifies the basic ring mode is reported correctly when supported.
TEST_CASE(
	"incremental: skip when kernel lacks IORING_FEAT_PBUF_RING_INC",
	"[incremental]") {
	Rig rig{4, 32};
	CHECK(rig.ring.mode() == BufferRingMode::incremental);
	CHECK(rig.caps.feat_pbuf_ring_inc);
}
// T8: Old-gen tombstone reclaim — conn_erase + stale terminal recv CQE →
//     tombstone consumed and buffer recycled exactly once.
//     [requires e2e HTTP server harness — tested via http_e2e integration tests]
TEST_CASE(
	"incremental: old-gen tombstone reclaim [e2e required]",
	"[incremental][.e2e]") {
	SKIP("tombstone table is internal to Ring struct in http_server.cxx; covered by e2e tests");
}
// T9: Partial-fill — two BUF_MORE CQEs for same buffer, bytes accumulate with
//     correct adjacent span layout.
TEST_CASE(
	"incremental: partial-fill: two BUF_MORE CQEs accumulate offsets",
	"[incremental]") {
	Rig rig{8, 64};
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	auto r1 = try_buffer_slice_from_incremental_cqe(rig.ring, 7, inc_flags(3, true));
	REQUIRE(r1.has_value());
	CHECK(r1->offset() == std::size_t{0});
	CHECK(r1->size() == std::size_t{7});
	CHECK(r1->bytes().size() == 7u);
	auto r2 = try_buffer_slice_from_incremental_cqe(rig.ring, 11, inc_flags(3, true));
	REQUIRE(r2.has_value());
	CHECK(r2->offset() == std::size_t{7});
	CHECK(r2->size() == std::size_t{11});
	CHECK(r2->bytes().size() == 11u);
	// Spans must be adjacent.
	CHECK(r2->bytes().data() == r1->bytes().data() + 7u);
	CHECK(rig.ring.debug_head_pos() == h0); // still BUF_MORE, head not advanced
}
// T10: Full-fill then next buffer — fill exactly buf_size, verify consumed,
//      next BUF_MORE on same ID starts at offset 0.
TEST_CASE(
	"incremental: full-fill then next buffer: consume + offset reset",
	"[incremental]") {
	Rig rig{8, 16}; // small buf_size for easier arithmetic
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	// Two CQEs filling all 16 bytes.
	auto r1 = try_buffer_slice_from_incremental_cqe(rig.ring, 8, inc_flags(0, true));
	REQUIRE(r1.has_value());
	CHECK(r1->offset() == std::size_t{0});
	auto r2 = try_buffer_slice_from_incremental_cqe(rig.ring, 8, inc_flags(0, false));
	REQUIRE(r2.has_value());
	CHECK(r2->offset() == std::size_t{8});
	CHECK(r2->size() == std::size_t{8});
	CHECK(rig.ring.debug_head_pos() == h0 + 1u); // final CQE: head advanced
	// Destructor recycles r2 (final). Next request for same ID gets fresh offset=0.
	r2 = IncrementalRecvSlice{}; // trigger dtor via move-assign from default
	auto r3 = try_buffer_slice_from_incremental_cqe(rig.ring, 4, inc_flags(0, true));
	REQUIRE(r3.has_value());
	CHECK(r3->offset() == std::size_t{0}); // new buffer acquisition, offset reset
	CHECK(r3->bytes().size() == 4u);
}
// T11: Classic vs incremental — same total bytes, different CQE count.
TEST_CASE(
	"incremental: classic vs incremental same data same outcome",
	"[incremental]") {
	Rig rig{8, 64}; // incremental, gid=0
	auto r_classic = conflux::uring::Ring::init(32, {});
	REQUIRE(r_classic);
	// Classic ring on gid=1 — no incremental cap needed.
	conflux::uring::IoUringCaps any_caps{};
	BufferRing classic_ring{
		r_classic->ref(),
		BufferRingOptions{
						  .count = 8,
						  .buf_size = 64,
						  .group_id = 1,
						  .huge_pages = false,
						  .mode = BufferRingMode::classic_one_cqe_per_buffer},
		any_caps
    };
	// Classic: 1 CQE → 15 bytes.
	std::size_t classic_total = 0;
	{
		auto slices = buffer_slices_from_cqe(classic_ring, 15, inc_flags(0, false), false);
		for (auto s: slices) {
			classic_total += s.bytes.size();
		}
		slices.recycle_all();
	}
	// Incremental: 3 CQEs → 5+5+5 = 15 bytes.
	std::size_t incremental_total = 0;
	{
		auto s1 = try_buffer_slice_from_incremental_cqe(rig.ring, 5, inc_flags(0, true));
		REQUIRE(s1);
		incremental_total += s1->size();
		auto s2 = try_buffer_slice_from_incremental_cqe(rig.ring, 5, inc_flags(0, true));
		REQUIRE(s2);
		incremental_total += s2->size();
		auto s3 = try_buffer_slice_from_incremental_cqe(rig.ring, 5, inc_flags(0, false));
		REQUIRE(s3);
		incremental_total += s3->size();
	}
	CHECK(classic_total == incremental_total);
	CHECK(classic_total == 15u);
}
// T12: WS handoff with active BUF_MORE [e2e required].
TEST_CASE(
	"incremental: WS handoff tombstone retire [e2e required]",
	"[incremental][.e2e]") {
	SKIP("begin_ws_handoff tombstone is internal to Ring; covered by e2e tests");
}
// T13: Invalid incremental CQE bounds — decoder returns bad_bounds, no mutation
//      of offset, head, or recycle state.
TEST_CASE(
	"incremental: invalid CQE bounds → bad_bounds, no offset/head mutation",
	"[incremental]") {
	Rig rig{8, 16}; // buf_size=16
	std::uint32_t const h0 = rig.ring.debug_head_pos();
	// Advance offset to 12 via a legitimate BUF_MORE.
	auto r1 = try_buffer_slice_from_incremental_cqe(rig.ring, 12, inc_flags(0, true));
	REQUIRE(r1.has_value());
	CHECK(r1->offset() == std::size_t{0});
	// Now try res=8: 12+8=20 > 16=buf_size → bad_bounds.
	auto r2 = try_buffer_slice_from_incremental_cqe(rig.ring, 8, inc_flags(0, false));
	REQUIRE(!r2.has_value());
	CHECK(r2.error() == RecvDecodeError::bad_bounds);
	// Offset must still be at 12 (not mutated).
	// Head must not have advanced.
	CHECK(rig.ring.debug_head_pos() == h0);
	// A valid continuation CQE at res=4 (12+4=16=buf_size) must still work.
	auto r3 = try_buffer_slice_from_incremental_cqe(rig.ring, 4, inc_flags(0, false));
	REQUIRE(r3.has_value());
	CHECK(r3->offset() == std::size_t{12});
	CHECK(rig.ring.debug_head_pos() == h0 + 1u);
}
// T14: Stale positive final CQE after tombstone [e2e required].
TEST_CASE(
	"incremental: stale positive final CQE clears tombstone [e2e required]",
	"[incremental][.e2e]") {
	SKIP("clear_retired_incremental_if_final is internal to Ring; covered by e2e tests");
}
