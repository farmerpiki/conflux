// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier.model_a;
import conflux.work.carrier.scope;

namespace root = conflux::work::root;
namespace model_a = conflux::work::carrier::model_a;
namespace carrier = conflux::work::carrier;

namespace {

model_a::Chain<int> make_success(
	int v) {
	auto [task, src] = root::make_task_source<int>();
	(void)src.commit_success(root::Success<int>{v});
	return model_a::from_task(std::move(task));
}

model_a::Chain<int> make_failure() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.commit_failure(std::make_exception_ptr(std::runtime_error{"fail"}));
	return model_a::from_task(std::move(task));
}

model_a::Chain<int> make_cancelled() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.commit_cancelled(root::CancelReason::shutdown);
	return model_a::from_task(std::move(task));
}

} // namespace

// ---------------------------------------------------------------------------
// when_all_fast_fail
// ---------------------------------------------------------------------------

TEST_CASE(
	"carrier.model_a: when_all_fast_fail both success returns Tup",
	"[carrier.model_a][phase2]") {
	auto r = model_a::when_all_fast_fail(make_success(10), make_success(20));
	auto out = std::move(r).release_outcome();
	REQUIRE(out.is_success());
	CHECK(std::get<0>(out.success().value) == 10);
	CHECK(std::get<1>(out.success().value) == 20);
}

TEST_CASE(
	"carrier.model_a: when_all_fast_fail a-failure returns failure",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::when_all_fast_fail(make_failure(), make_success(1))).release_outcome();
	CHECK(out.is_failure());
}

TEST_CASE(
	"carrier.model_a: when_all_fast_fail b-failure returns failure",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::when_all_fast_fail(make_success(1), make_failure())).release_outcome();
	CHECK(out.is_failure());
}

TEST_CASE(
	"carrier.model_a: when_all_fast_fail a-cancel returns cancel",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::when_all_fast_fail(make_cancelled(), make_success(1))).release_outcome();
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"carrier.model_a: when_all_fast_fail b-cancel returns cancel",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::when_all_fast_fail(make_success(1), make_cancelled())).release_outcome();
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"carrier.model_a: when_all_fast_fail failure beats cancel",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::when_all_fast_fail(make_failure(), make_cancelled())).release_outcome();
	CHECK(out.is_failure());
}

// ---------------------------------------------------------------------------
// race
// ---------------------------------------------------------------------------

TEST_CASE(
	"carrier.model_a: race a-wins when a is success",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::race(make_success(7), make_success(99))).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 7);
}

TEST_CASE(
	"carrier.model_a: race b-wins when only b is success",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::race(make_failure(), make_success(5))).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 5);
}

TEST_CASE(
	"carrier.model_a: race success beats cancel (b wins)",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::race(make_cancelled(), make_success(3))).release_outcome();
	CHECK(out.is_success());
}

TEST_CASE(
	"carrier.model_a: race failure beats cancel",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::race(make_cancelled(), make_failure())).release_outcome();
	CHECK(out.is_failure());
}

TEST_CASE(
	"carrier.model_a: race both fail returns a failure",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::race(make_failure(), make_failure())).release_outcome();
	CHECK(out.is_failure());
}

TEST_CASE(
	"carrier.model_a: race both cancel returns a cancel",
	"[carrier.model_a][phase2]") {
	auto out = std::move(model_a::race(make_cancelled(), make_cancelled())).release_outcome();
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"carrier.model_a: race preserves a kind on a-win",
	"[carrier.model_a][phase2]") {
	auto r = model_a::race(make_success(1), make_success(2));
	CHECK(r.kind() == model_a::CarrierKind::task);
	auto out = std::move(r).release_outcome();
	CHECK(out.success().value == 1);
}

// ---------------------------------------------------------------------------
// Scope — state and control propagation
// ---------------------------------------------------------------------------

TEST_CASE(
	"carrier.scope: initial state is not cancelled",
	"[carrier.scope]") {
	carrier::Scope const scope{};
	CHECK(!scope.is_cancelled());
}

TEST_CASE(
	"carrier.scope: cancel marks scope cancelled with reason",
	"[carrier.scope]") {
	carrier::Scope scope{};
	scope.cancel(root::CancelReason::shutdown);
	CHECK(scope.is_cancelled());
	CHECK(scope.cancel_reason() == root::CancelReason::shutdown);
}

TEST_CASE(
	"carrier.scope: cancel is idempotent, first reason wins",
	"[carrier.scope]") {
	carrier::Scope scope{};
	scope.cancel(root::CancelReason::shutdown);
	scope.cancel(root::CancelReason::requested);
	CHECK(scope.cancel_reason() == root::CancelReason::shutdown);
}

TEST_CASE(
	"carrier.scope: track then cancel propagates via stop_token",
	"[carrier.scope]") {
	auto [ctrl, src] = root::make_task_control_source<int>();
	auto token = src.stop_token();
	CHECK(!token.stop_requested());

	carrier::Scope scope{};
	scope.track(std::move(ctrl));
	CHECK(!token.stop_requested());

	scope.cancel(root::CancelReason::requested);
	CHECK(token.stop_requested());
}

TEST_CASE(
	"carrier.scope: track after cancel fires request_cancel immediately",
	"[carrier.scope]") {
	auto [ctrl, src] = root::make_task_control_source<int>();
	auto token = src.stop_token();

	carrier::Scope scope{};
	scope.cancel(root::CancelReason::requested);
	CHECK(!token.stop_requested());

	scope.track(std::move(ctrl));
	CHECK(token.stop_requested());
}

// ---------------------------------------------------------------------------
// Scope — admit
// ---------------------------------------------------------------------------

TEST_CASE(
	"carrier.scope: admit success task returns success chain",
	"[carrier.scope]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.commit_success(root::Success<int>{42});
	auto jh = root::into_join_handle(std::move(task));

	carrier::Scope scope{};
	auto chain = scope.admit(std::move(jh));
	auto out = std::move(chain).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 42);
}

TEST_CASE(
	"carrier.scope: admit already-cancelled task returns cancelled chain",
	"[carrier.scope]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.commit_cancelled(root::CancelReason::shutdown);
	auto jh = root::into_join_handle(std::move(task));

	carrier::Scope scope{};
	auto chain = scope.admit(std::move(jh));
	auto out = std::move(chain).release_outcome();
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"carrier.scope: admit after scope-cancel signals task then joins",
	"[carrier.scope]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(std::move(task));
	std::mutex mu;
	std::condition_variable cv;
	bool cancel_seen = false;

	(void)src.install_cancel_hook([&](root::CancelReason) {
		{
			std::lock_guard const lock{mu};
			cancel_seen = true;
		}
		cv.notify_one();
	});

	carrier::Scope scope{};
	scope.cancel(root::CancelReason::requested);

	auto worker_src = std::move(src);
	std::thread worker{[&mu, &cv, &cancel_seen, ws = std::move(worker_src)]() mutable {
		std::unique_lock lock{mu};
		bool const observed = cv.wait_for(lock, std::chrono::seconds{1}, [&] { return cancel_seen; });
		lock.unlock();
		if (observed) {
			(void)ws.commit_cancelled(root::CancelReason::requested);
		} else {
			(void)ws.commit_failure(
				std::make_exception_ptr(std::runtime_error{"scope admit did not signal cancellation"}));
		}
	}};

	auto chain = scope.admit(std::move(jh));
	worker.join();

	auto out = std::move(chain).release_outcome();
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"carrier.scope: concurrent cancel during admit unblocks join",
	"[carrier.scope]") {
	auto [task, src] = root::make_task_source<int>();
	auto stop_token = src.stop_token();
	auto jh = root::into_join_handle(std::move(task));

	carrier::Scope scope{};

	auto canceller_src = std::move(src);
	auto canceller_token = stop_token;
	// Cancels scope after a brief delay, then commits cancelled
	std::thread canceller{[&scope, cs = std::move(canceller_src), ct = std::move(canceller_token)]() mutable {
		std::this_thread::sleep_for(std::chrono::milliseconds{5});
		scope.cancel(root::CancelReason::requested);
		while (!ct.stop_requested()) {
			std::this_thread::yield();
		}
		(void)cs.commit_cancelled(root::CancelReason::requested);
	}};

	auto chain = scope.admit(std::move(jh));
	canceller.join();

	auto out = std::move(chain).release_outcome();
	CHECK(out.is_cancelled());
}
