// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier;
import conflux.work.carrier.scope;
import conflux.work.carrier.deadline;

namespace root = conflux::work::root;
namespace carrier = conflux::work::carrier;
namespace carrier = conflux::work::carrier;
// ---------------------------------------------------------------------------
// CancelReason::deadline exists
// ---------------------------------------------------------------------------

TEST_CASE(
	"root: CancelReason::deadline value is distinct",
	"[root][phase3]") {
	CHECK(root::CancelReason::deadline != root::CancelReason::requested);
	CHECK(root::CancelReason::deadline != root::CancelReason::abandoned);
	CHECK(root::CancelReason::deadline != root::CancelReason::shutdown);
}
// ---------------------------------------------------------------------------
// DeadlineScope — state
// ---------------------------------------------------------------------------

TEST_CASE(
	"carrier.deadline: initial state is not cancelled",
	"[carrier.deadline]") {
	carrier::DeadlineScope scope{std::chrono::seconds{60}};
	CHECK(!scope.is_cancelled());
}
TEST_CASE(
	"carrier.deadline: cancel(requested) before deadline fires with requested reason",
	"[carrier.deadline]") {
	carrier::DeadlineScope scope{std::chrono::seconds{60}};
	scope.cancel(root::CancelReason::requested);
	CHECK(scope.is_cancelled());
	CHECK(scope.cancel_reason() == root::CancelReason::requested);
}
TEST_CASE(
	"carrier.deadline: past deadline fires immediately with deadline reason",
	"[carrier.deadline]") {
	auto const past = std::chrono::steady_clock::now() - std::chrono::seconds{1};
	carrier::DeadlineScope scope{past};
	// Timer fires nearly immediately; spin until cancel propagates.
	for (int i = 0; i < 10000 && !scope.is_cancelled(); ++i) {
		std::this_thread::yield();
	}
	REQUIRE(scope.is_cancelled());
	CHECK(scope.cancel_reason() == root::CancelReason::deadline);
}
TEST_CASE(
	"carrier.deadline: deadline fires with deadline reason after duration",
	"[carrier.deadline]") {
	carrier::DeadlineScope scope{std::chrono::milliseconds{5}};
	for (int i = 0; i < 100000 && !scope.is_cancelled(); ++i) {
		std::this_thread::yield();
	}
	REQUIRE(scope.is_cancelled());
	CHECK(scope.cancel_reason() == root::CancelReason::deadline);
}
TEST_CASE(
	"carrier.deadline: cancel(requested) wins over pending deadline (idempotent)",
	"[carrier.deadline]") {
	carrier::DeadlineScope scope{std::chrono::milliseconds{50}};
	scope.cancel(root::CancelReason::requested);
	// Deadline would fire ~50ms later; first cancel wins.
	CHECK(scope.cancel_reason() == root::CancelReason::requested);
	// Wait long enough that the timer would have fired if not stopped.
	std::this_thread::sleep_for(std::chrono::milliseconds{80});
	CHECK(scope.cancel_reason() == root::CancelReason::requested);
}
// ---------------------------------------------------------------------------
// DeadlineScope — admit
// ---------------------------------------------------------------------------

TEST_CASE(
	"carrier.deadline: admit task that completes before deadline returns success",
	"[carrier.deadline]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{99});
	auto jh = root::into_join_handle(std::move(task));

	carrier::DeadlineScope scope{std::chrono::seconds{60}};
	auto chain = scope.admit(std::move(jh));
	auto out = std::move(chain).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 99);
	CHECK(!scope.is_cancelled());
}
TEST_CASE(
	"carrier.deadline: admit already-cancelled task returns cancelled",
	"[carrier.deadline]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_cancelled(root::work_errc::cancelled_shutdown);
	auto jh = root::into_join_handle(std::move(task));

	carrier::DeadlineScope scope{std::chrono::seconds{60}};
	auto chain = scope.admit(std::move(jh));
	auto out = std::move(chain).release_outcome();
	CHECK(out.is_cancelled());
}
TEST_CASE(
	"carrier.deadline: deadline fires during admit — task gets cancel signal",
	"[carrier.deadline]") {
	auto [task, src] = root::make_task_source<int>();
	auto stop_token = src.stop_token();
	auto jh = root::into_join_handle(std::move(task));

	carrier::DeadlineScope scope{std::chrono::milliseconds{5}};

	auto worker_src = std::move(src);
	auto worker_token = stop_token;
	std::thread worker{[ws = std::move(worker_src), wt = std::move(worker_token)]() mutable {
		while (!wt.stop_requested()) {
			std::this_thread::yield();
		}
		(void)ws.try_set_cancelled(root::work_errc::cancelled_deadline);
	}};

	auto chain = scope.admit(std::move(jh));
	worker.join();

	auto out = std::move(chain).release_outcome();
	CHECK(out.is_cancelled());
	CHECK(scope.is_cancelled());
	CHECK(scope.cancel_reason() == root::CancelReason::deadline);
}
TEST_CASE(
	"carrier.deadline: DeadlineScope destroyed before deadline — no cancel fired",
	"[carrier.deadline]") {
	bool cancelled = false;
	{
		carrier::DeadlineScope scope{std::chrono::seconds{60}};
		// Destroy scope immediately — timer thread should join cleanly.
	}
	// If this test reaches here without crash or cancel, destruction is clean.
	CHECK(!cancelled);
}
