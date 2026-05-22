// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.uring.completion;

using namespace conflux::uring;
namespace {

Ring make_ring(
	unsigned sq = 16) {
	auto r = Ring::init(sq, {});
	REQUIRE(r);
	return std::move(*r);
}
// Submit n NOP SQEs in batches of sq_size without draining the CQ.
void submit_nops_no_drain(
	Ring &ring,
	unsigned n,
	unsigned sq_size) {
	unsigned submitted = 0;
	while (submitted < n) {
		unsigned const batch = std::min(n - submitted, sq_size);
		for (unsigned j = 0; j < batch; ++j) {
			auto sqe = ring.get_sqe();
			if (!sqe) {
				break;
			}
			sqe.prep_nop().user_data(UserData{static_cast<std::uint64_t>(submitted + j)});
		}
		ring.submit();
		submitted += batch;
	}
}
// Drain all visible CQEs without blocking.
unsigned drain_cq(
	Ring &ring) {
	unsigned n = 0;
	io_uring_cqe *cqe = nullptr;
	while (ring.peek_cqe(&cqe) == 0 && cqe != nullptr) {
		ring.cqe_seen(cqe);
		++n;
	}
	return n;
}

} // namespace
// ── API unit tests ────────────────────────────────────────────────────────────

TEST_CASE(
	"uring.cq_overflow_count: zero on fresh ring",
	"[cq_overflow]") {
	auto ring = make_ring();
	CHECK(ring.cq_has_overflow() == false);
	CHECK(ring.cq_overflow_count() == 0u);
}
TEST_CASE(
	"uring.RingRef.has_feature: consistent with Ring::has_feature",
	"[cq_overflow]") {
	auto ring = make_ring();
	RingRef ref = ring.ref();
	CHECK(ring.has_feature(IORING_FEAT_NODROP) == ref.has_feature(IORING_FEAT_NODROP));
	CHECK(ring.has_feature(IORING_FEAT_FAST_POLL) == ref.has_feature(IORING_FEAT_FAST_POLL));
}
TEST_CASE(
	"uring.RingRef.cq_overflow_count: matches Ring",
	"[cq_overflow]") {
	auto ring = make_ring();
	RingRef ref = ring.ref();
	CHECK(ring.cq_overflow_count() == ref.cq_overflow_count());
}
TEST_CASE(
	"uring.CompletionTable: dispatch accepts move-only callbacks",
	"[completion]") {
	CompletionTable completions{1};
	auto owned = std::make_unique<int>(41);
	int observed = 0;

	auto [slot, gen] = completions.reserve([ptr = std::move(owned), &observed](IoResult r) mutable {
		observed = *ptr + r.res;
		ptr.reset();
	});

	completions.dispatch(slot, gen, 1, conflux::uring::CqeFlags{});

	CHECK(observed == 42);
	CHECK(completions.pending() == 0u);
}
TEST_CASE(
	"uring.cq_overflow_count: koverflow null-safe",
	"[cq_overflow]") {
	// Both Ring and RingRef must not crash on a live ring.
	auto ring = make_ring();
	RingRef ref = ring.ref();
	CHECK(ring.cq_overflow_count() == 0u);
	CHECK(ring.cq_overflow_count() == ref.cq_overflow_count());
}
TEST_CASE(
	"uring.cq_has_overflow: no overflow when CQ drained",
	"[cq_overflow]") {
	auto ring = make_ring(64);
	static constexpr unsigned n_ops = 16;
	for (unsigned i = 0; i < n_ops; ++i) {
		auto sqe = ring.get_sqe();
		REQUIRE(sqe);
		sqe.prep_nop().user_data(UserData{static_cast<std::uint64_t>(i)});
	}
	ring.submit();
	for (unsigned d = 0; d < n_ops;) {
		io_uring_cqe *cqe = nullptr;
		ring.wait_cqe(&cqe);
		ring.cqe_seen(cqe);
		++d;
	}
	CHECK(ring.cq_has_overflow() == false);
	CHECK(ring.cq_overflow_count() == 0u);
	CHECK(ring.cq_overflow_count() == ring.ref().cq_overflow_count());
}
// ── Deterministic overflow tests ──────────────────────────────────────────────
//
// Ring: SQ=4, CQ=8 (default 2×SQ).
// Submit 12 NOPs in 3 batches of 4 without draining CQ.
// NOPs complete synchronously inside io_uring_enter, so all 12 are in-flight
// before submit() returns. 8 fill the CQ ring; 4 go to the overflow list.
//
// NODROP semantics (modern kernels):
//   - koverflow (cq_overflow_count) counts _dropped_ CQEs — with NODROP no
//     CQEs are dropped, so koverflow stays 0 even during overflow.
//   - The overflow list is automatically flushed back into the CQ as userspace
//     drains slots; the drain loop therefore yields all 12 CQEs.
//   - cq_has_overflow() reads IORING_SQ_CQ_OVERFLOW (a flag, not the counter)
//     and is the reliable indicator of an active overflow list.
//
// Without NODROP:
//   - Excess CQEs are silently dropped; koverflow increments per dropped CQE.
//   - cq_has_overflow() is unreliable — overflow means data loss.

TEST_CASE(
	"uring.cq_overflow: submit past CQ depth triggers overflow",
	"[cq_overflow]") {
	// SQ=4 → CQ=8 (2×SQ default)
	auto ring_exp = Ring::init(4, {});
	REQUIRE(ring_exp);
	auto ring = std::move(*ring_exp);

	// Verify NODROP: influences how we interpret koverflow.
	bool const nodrop = ring.has_feature(IORING_FEAT_NODROP);
	CHECK(ring.ref().has_feature(IORING_FEAT_NODROP) == nodrop);

	// Submit 12 NOPs (3 batches of 4) — 4 must overflow CQ-8.
	submit_nops_no_drain(ring, 12, 4);

	// IORING_SQ_CQ_OVERFLOW flag set immediately when overflow list is non-empty.
	REQUIRE(ring.cq_has_overflow());
	CHECK(ring.ref().cq_has_overflow()); // RingRef agrees

	if (nodrop) {
		// With NODROP: nothing dropped → koverflow stays 0.
		// The overflow list holds the 4 extra CQEs safely.
		CHECK(ring.cq_overflow_count() == 0u);
		CHECK(ring.ref().cq_overflow_count() == 0u);
	} else {
		// Without NODROP: 4 CQEs were dropped → koverflow increments.
		CHECK(ring.cq_overflow_count() > 0u);
		CHECK(ring.ref().cq_overflow_count() == ring.cq_overflow_count());
	}

	// Drain all CQEs. With NODROP the kernel flushes the overflow list into
	// the CQ as slots free up; the drain loop recovers all 12 CQEs.
	unsigned const drained = drain_cq(ring);
	if (nodrop) {
		CHECK(drained == 12u); // all 12 preserved and delivered
	}

	// After full drain the overflow list is empty.
	CHECK(ring.cq_has_overflow() == false);
	CHECK(ring.ref().cq_has_overflow() == false);
	// koverflow: with NODROP still 0 (no drops occurred).
	//            without NODROP still > 0 (dropped CQEs counted).
	if (nodrop) {
		CHECK(ring.cq_overflow_count() == 0u);
	} else {
		CHECK(ring.cq_overflow_count() > 0u);
	}
}
TEST_CASE(
	"uring.cq_overflow_count: monotonically non-decreasing (without NODROP only)",
	"[cq_overflow]") {
	auto ring_exp = Ring::init(4, {});
	REQUIRE(ring_exp);
	auto ring = std::move(*ring_exp);

	if (ring.has_feature(IORING_FEAT_NODROP)) {
		SKIP("koverflow counts dropped CQEs only; with NODROP no drops occur — counter stays 0");
	}

	// First overflow round.
	submit_nops_no_drain(ring, 12, 4);
	REQUIRE(ring.cq_has_overflow());
	std::uint32_t const first = ring.cq_overflow_count();
	CHECK(first > 0u);
	drain_cq(ring);

	// Second overflow round.
	submit_nops_no_drain(ring, 12, 4);
	REQUIRE(ring.cq_has_overflow());
	std::uint32_t const second = ring.cq_overflow_count();
	CHECK(second >= first);
	drain_cq(ring);
}
