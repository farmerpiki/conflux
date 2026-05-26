// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.small_function;
import conflux.work.root;
import conflux.work.carrier;
import conflux.work.race;

namespace root = conflux::work::root;
namespace carrier = conflux::work::carrier;
namespace race = conflux::work::race;

namespace {

struct OwnerCap {};
struct DriverCap {};

} // namespace
namespace conflux::work::root {

template<>
inline constexpr bool enable_address_capability_v<OwnerCap> = true;
template<>
inline constexpr bool enable_address_capability_v<DriverCap> = true;

} // namespace conflux::work::root

TEST_CASE(
	"work.race: empty participant list fails setup",
	"[work.race]") {
	auto raced = race::race<int>(race::race_options{});

	try {
		(void)root::value(std::move(raced));
		FAIL("race setup should fail");
	} catch (root::FailureError const &err) { CHECK_THROWS_AS(err.rethrow_cause(), race::race_setup_error); }
}

TEST_CASE(
	"work.race: first completion wins",
	"[work.race]") {
	auto [slow, slow_src] = root::make_task_source<int>();
	auto [fast, fast_src] = root::make_task_source<int>();

	auto raced = race::race<int>(
		race::race_options{.winner = race::winner_policy::first_completion},
		race::candidate("slow", std::move(slow)),
		race::candidate("fast", std::move(fast)));

	REQUIRE(fast_src.try_set_value(root::Success<int>{7}));
	REQUIRE(slow_src.try_set_value(root::Success<int>{3}));

	auto out = root::value(std::move(raced));
	CHECK(out.winner.index == 1);
	CHECK(out.winner.label == "fast");
	CHECK(out.observation.participant_count == 2);
	CHECK(out.observation.loser_cancel_requested == 1);
	CHECK_FALSE(out.observation.trigger_won);
	CHECK_FALSE(out.observation.all_failed);
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 7);
}

TEST_CASE(
	"work.race: candidate_on joins posted participant with owner capability",
	"[work.race]") {
	OwnerCap owner{};
	auto [posted, posted_src] = root::make_posted_source<int>(owner);
	auto [task, task_src] = root::make_task_source<int>();

	auto raced = race::race<int>(
		race::race_options{},
		race::candidate_on(owner, "posted", std::move(posted)),
		race::candidate("task", std::move(task)));

	REQUIRE(posted_src.try_set_value(root::Success<int>{31}));
	REQUIRE(task_src.try_set_cancelled(root::CancelReason::requested));
	auto out = root::value(std::move(raced));
	CHECK(out.winner.label == "posted");
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 31);
}

TEST_CASE(
	"work.race: candidate_on reports capability mismatch from owner-bound participant",
	"[work.race]") {
	OwnerCap owner{};
	OwnerCap other{};
	auto [posted, posted_src] = root::make_posted_source<int>(owner);

	auto raced = race::race<int>(race::race_options{}, race::candidate_on(other, "posted", std::move(posted)));

	REQUIRE(posted_src.try_set_value(root::Success<int>{9}));
	auto out = root::value(std::move(raced));
	REQUIRE(out.outcome.is_failure());
	CHECK_THROWS_AS(std::rethrow_exception(out.outcome.failure().error), root::JoinError);
}

TEST_CASE(
	"work.race: candidate_on joins operation participant with driver capability",
	"[work.race]") {
	DriverCap driver{};
	auto [op, op_src] = root::make_operation_source<int>(driver);

	auto raced = race::race<int>(race::race_options{}, race::candidate_on(driver, "operation", std::move(op)));

	REQUIRE(op_src.try_set_value(root::Success<int>{17}));
	auto out = root::value(std::move(raced));
	CHECK(out.winner.label == "operation");
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 17);
}

TEST_CASE(
	"work.race: first success ignores early failure",
	"[work.race]") {
	auto [bad, bad_src] = root::make_task_source<int>();
	auto [good, good_src] = root::make_task_source<int>();

	auto raced = race::race<int>(
		race::race_options{.winner = race::winner_policy::first_success},
		race::candidate("bad", std::move(bad)),
		race::candidate("good", std::move(good)));

	REQUIRE(bad_src.try_set_exception(std::make_exception_ptr(std::runtime_error{"bad"})));
	CHECK_FALSE(raced.control().ready());

	REQUIRE(good_src.try_set_value(root::Success<int>{42}));
	auto out = root::value(std::move(raced));
	CHECK(out.winner.index == 1);
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 42);
}

TEST_CASE(
	"work.race: trigger wins with cancellation reason",
	"[work.race]") {
	auto [value, value_src] = root::make_task_source<int>();
	auto [deadline, deadline_src] = root::make_task_source<void>();

	auto raced = race::race<int>(
		race::race_options{},
		race::candidate("value", std::move(value)),
		race::trigger("deadline", std::move(deadline), root::CancelReason::deadline));

	REQUIRE(deadline_src.try_set_value());
	REQUIRE(value_src.try_set_value(root::Success<int>{1}));

	auto out = root::value(std::move(raced));
	CHECK(out.winner.index == 1);
	CHECK(out.winner.kind == race::race_winner_kind::trigger);
	CHECK(out.observation.participant_count == 2);
	CHECK(out.observation.loser_cancel_requested == 1);
	CHECK(out.observation.trigger_won);
	CHECK_FALSE(out.observation.all_failed);
	REQUIRE(out.outcome.is_cancelled());
	CHECK(out.outcome.cancelled().reason == root::CancelReason::deadline);
}

TEST_CASE(
	"work.race: stop token trigger wins with shutdown reason",
	"[work.race]") {
	std::stop_source stop{};
	auto [value, value_src] = root::make_task_source<int>();

	auto raced = race::race<int>(
		race::race_options{.losers = race::loser_policy::request_cancel},
		race::candidate("value", std::move(value)),
		race::until_stop_token(stop.get_token()));

	REQUIRE(stop.request_stop());

	auto out = root::value(std::move(raced));
	CHECK(out.winner.label == "stop_token");
	CHECK(out.winner.kind == race::race_winner_kind::trigger);
	REQUIRE(out.outcome.is_cancelled());
	CHECK(out.outcome.cancelled().reason == root::CancelReason::shutdown);
	REQUIRE(value_src.try_set_cancelled(root::CancelReason::requested));
}

TEST_CASE(
	"work.race: losing stop token trigger is cancellable",
	"[work.race]") {
	std::stop_source stop{};
	auto [value, value_src] = root::make_task_source<int>();

	auto raced = race::race<int>(
		race::race_options{},
		race::candidate("value", std::move(value)),
		race::until_stop_token(stop.get_token()));

	REQUIRE(value_src.try_set_value(root::Success<int>{71}));

	auto out = root::value(std::move(raced));
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 71);
	CHECK(out.observation.loser_cancel_requested == 1);
}

TEST_CASE(
	"work.race: trigger_on maps owner-bound void work to trigger",
	"[work.race]") {
	OwnerCap owner{};
	auto [value, value_src] = root::make_task_source<int>();
	auto [deadline, deadline_src] = root::make_posted_source<void>(owner);

	auto raced = race::race<int>(
		race::race_options{},
		race::candidate("value", std::move(value)),
		race::trigger_on(owner, "deadline", std::move(deadline), root::CancelReason::deadline));

	REQUIRE(deadline_src.try_set_value());
	REQUIRE(value_src.try_set_cancelled(root::CancelReason::requested));
	auto out = root::value(std::move(raced));
	CHECK(out.winner.label == "deadline");
	CHECK(out.winner.kind == race::race_winner_kind::trigger);
	REQUIRE(out.outcome.is_cancelled());
	CHECK(out.outcome.cancelled().reason == root::CancelReason::deadline);
}

TEST_CASE(
	"work.race: trigger_on reports owner-bound capability mismatch",
	"[work.race]") {
	OwnerCap owner{};
	OwnerCap other{};
	auto [posted, posted_src] = root::make_posted_source<void>(owner);

	auto raced = race::race<int>(
		race::race_options{},
		race::trigger_on(other, "deadline", std::move(posted), root::CancelReason::deadline));

	REQUIRE(posted_src.try_set_value());
	auto out = root::value(std::move(raced));
	REQUIRE(out.outcome.is_failure());
	CHECK_THROWS_AS(std::rethrow_exception(out.outcome.failure().error), root::JoinError);
}

TEST_CASE(
	"work.race: with_timeout maps timeout task to deadline trigger",
	"[work.race]") {
	auto [work, work_src] = root::make_task_source<int>();
	auto [deadline, deadline_src] = root::make_task_source<void>();

	auto raced = race::with_timeout<int>(
		std::move(work),
		std::move(deadline),
		race::race_options{.losers = race::loser_policy::request_cancel});

	REQUIRE(deadline_src.try_set_value());
	auto out = root::value(std::move(raced));
	CHECK(out.winner.label == "deadline");
	CHECK(out.winner.kind == race::race_winner_kind::trigger);
	REQUIRE(out.outcome.is_cancelled());
	CHECK(out.outcome.cancelled().reason == root::CancelReason::deadline);
	REQUIRE(work_src.try_set_cancelled(root::CancelReason::requested));
}

TEST_CASE(
	"work.race: ready chain can win",
	"[work.race]") {
	auto [task, src] = root::make_task_source<int>();
	auto chain = carrier::Chain<int>{root::Outcome<int>{root::Success<int>{5}}, carrier::CarrierKind::task};

	auto raced = race::race<int>(
		race::race_options{},
		race::candidate("memory", std::move(chain)),
		race::candidate("task", std::move(task)));

	REQUIRE(src.try_set_value(root::Success<int>{9}));
	auto out = root::value(std::move(raced));
	CHECK(out.winner.index == 0);
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 5);
}

TEST_CASE(
	"work.race: request_cancel cancels loser without waiting for loser terminal completion",
	"[work.race]") {
	auto [winner, winner_src] = root::make_task_source<int>();
	auto [loser, loser_src] = root::make_task_source<int>();
	auto loser_control = loser.control();

	auto raced = race::race<int>(
		race::race_options{.losers = race::loser_policy::request_cancel},
		race::candidate("winner", std::move(winner)),
		race::candidate("loser", std::move(loser)));

	REQUIRE(winner_src.try_set_value(root::Success<int>{11}));
	auto out = root::value(std::move(raced));
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 11);
	CHECK(loser_control.cancel_requested());

	REQUIRE(loser_src.try_set_cancelled(root::CancelReason::requested));
}

TEST_CASE(
	"work.race: leave_running keeps consumed loser owned until terminal completion",
	"[work.race]") {
	auto [winner, winner_src] = root::make_task_source<int>();
	auto [loser, loser_src] = root::make_task_source<int>();
	auto loser_control = loser.control();

	auto raced = race::race<int>(
		race::race_options{.losers = race::loser_policy::leave_running},
		race::candidate("winner", std::move(winner)),
		race::candidate("loser", std::move(loser)));

	REQUIRE(winner_src.try_set_value(root::Success<int>{23}));
	auto out = root::value(std::move(raced));
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 23);
	CHECK_FALSE(loser_control.cancel_requested());

	REQUIRE(loser_src.try_set_value(root::Success<int>{99}));
}

TEST_CASE(
	"work.race: request_cancel keeps consumed loser owned after winner returns",
	"[work.race]") {
	auto [winner, winner_src] = root::make_task_source<int>();
	auto [loser, loser_src] = root::make_task_source<int>();
	auto loser_control = loser.control();

	auto raced = race::race<int>(
		race::race_options{.losers = race::loser_policy::request_cancel},
		race::candidate("winner", std::move(winner)),
		race::candidate("loser", std::move(loser)));

	REQUIRE(winner_src.try_set_value(root::Success<int>{41}));
	auto out = root::value(std::move(raced));
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 41);
	CHECK(loser_control.cancel_requested());

	REQUIRE(loser_src.try_set_exception(std::make_exception_ptr(std::runtime_error{"late loser failure"})));
}

TEST_CASE(
	"work.race: collect_loser_outcomes records drained loser outcomes",
	"[work.race]") {
	auto [winner, winner_src] = root::make_task_source<int>();
	auto [loser, loser_src] = root::make_task_source<int>();

	auto raced = race::race<int>(
		race::race_options{
			.losers = race::loser_policy::request_cancel_and_wait,
			.collect_loser_outcomes = true,
		},
		race::candidate("winner", std::move(winner)),
		race::candidate("loser", std::move(loser)));

	REQUIRE(winner_src.try_set_value(root::Success<int>{53}));
	CHECK_FALSE(raced.control().ready());
	REQUIRE(loser_src.try_set_cancelled(root::CancelReason::requested));

	auto out = root::value(std::move(raced));
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 53);
	REQUIRE(out.loser_outcomes.size() == 1);
	CHECK(out.loser_outcomes[0].index == 1);
	CHECK(out.loser_outcomes[0].label == "loser");
	REQUIRE(out.loser_outcomes[0].outcome.is_cancelled());
	CHECK(out.loser_outcomes[0].outcome.cancelled().reason == root::CancelReason::requested);
}

TEST_CASE(
	"work.race: external cancellation forwards reason to live participants",
	"[work.race]") {
	auto [left, left_src] = root::make_task_source<int>();
	auto [right, right_src] = root::make_task_source<int>();
	auto left_control = left.control();
	auto right_control = right.control();

	auto raced = race::race<int>(
		race::race_options{},
		race::candidate("left", std::move(left)),
		race::candidate("right", std::move(right)));

	raced.cancel(root::CancelReason::deadline);
	CHECK(left_control.cancel_requested());
	CHECK(right_control.cancel_requested());
	CHECK(left_control.cancellation_reason() == root::CancelReason::deadline);
	CHECK(right_control.cancellation_reason() == root::CancelReason::deadline);

	try {
		(void)root::value(std::move(raced));
		FAIL("race should be cancelled");
	} catch (root::CancelledError const &err) { CHECK(err.reason() == root::CancelReason::deadline); }

	REQUIRE(left_src.try_set_cancelled(root::CancelReason::deadline));
	REQUIRE(right_src.try_set_cancelled(root::CancelReason::deadline));
}

TEST_CASE(
	"work.race: nested race cancellation cascades into inner participants",
	"[work.race]") {
	auto [inner_left, inner_left_src] = root::make_task_source<int>();
	auto [inner_right, inner_right_src] = root::make_task_source<int>();
	auto inner_left_control = inner_left.control();
	auto inner_right_control = inner_right.control();
	auto [outer_other, outer_other_src] = root::make_task_source<race::race_result<int>>();
	auto outer_other_control = outer_other.control();

	auto inner = race::race<int>(
		race::race_options{},
		race::candidate("inner-left", std::move(inner_left)),
		race::candidate("inner-right", std::move(inner_right)));
	auto outer = race::race<race::race_result<int>>(
		race::race_options{},
		race::candidate("inner", std::move(inner)),
		race::candidate("other", std::move(outer_other)));

	outer.cancel(root::CancelReason::shutdown);
	CHECK(inner_left_control.cancel_requested());
	CHECK(inner_right_control.cancel_requested());
	CHECK(outer_other_control.cancel_requested());
	CHECK(inner_left_control.cancellation_reason() == root::CancelReason::shutdown);
	CHECK(inner_right_control.cancellation_reason() == root::CancelReason::shutdown);
	CHECK(outer_other_control.cancellation_reason() == root::CancelReason::shutdown);

	try {
		(void)root::value(std::move(outer));
		FAIL("outer race should be cancelled");
	} catch (root::CancelledError const &err) { CHECK(err.reason() == root::CancelReason::shutdown); }

	REQUIRE(inner_left_src.try_set_cancelled(root::CancelReason::shutdown));
	REQUIRE(inner_right_src.try_set_cancelled(root::CancelReason::shutdown));
	REQUIRE(outer_other_src.try_set_cancelled(root::CancelReason::shutdown));
}

TEST_CASE(
	"work.race: registration failure cancels already-owned participants",
	"[work.race]") {
	auto [owned, owned_src] = root::make_task_source<int>();
	auto owned_control = owned.control();
	auto [shared, shared_src] = root::make_task_source<int>();
	(void)shared.control().try_set_on_ready(::conflux::detail::small_move_only_function<void()>{[]() noexcept {}});

	auto raced = race::race<int>(
		race::race_options{},
		race::candidate("owned", std::move(owned)),
		race::candidate("shared", std::move(shared)));

	CHECK(owned_control.cancel_requested());
	try {
		(void)root::value(std::move(raced));
		FAIL("race setup should fail");
	} catch (root::FailureError const &err) { CHECK_THROWS_AS(err.rethrow_cause(), race::race_setup_error); }

	REQUIRE(owned_src.try_set_cancelled(root::CancelReason::requested));
	REQUIRE(shared_src.try_set_cancelled(root::CancelReason::requested));
}

TEST_CASE(
	"work.race: cleanup budget expires when loser ignores cancellation",
	"[work.race]") {
	auto [winner, winner_src] = root::make_task_source<int>();
	auto [loser, loser_src] = root::make_task_source<int>();
	auto loser_control = loser.control();

	auto raced = race::race<int>(
		race::race_options{
			.cleanup = race::loser_cleanup_policy::fail_after_cleanup_deadline,
			.loser_cleanup_budget = std::chrono::milliseconds{1},
		},
		race::candidate("winner", std::move(winner)),
		race::candidate("loser", std::move(loser)));

	REQUIRE(winner_src.try_set_value(root::Success<int>{7}));
	CHECK(loser_control.cancel_requested());
	try {
		(void)root::value(std::move(raced));
		FAIL("cleanup budget should expire");
	} catch (root::FailureError const &err) { CHECK_THROWS_AS(err.rethrow_cause(), race::race_cleanup_error); }

	REQUIRE(loser_src.try_set_cancelled(root::CancelReason::requested));
}

TEST_CASE(
	"work.race: cleanup budget does not fire after loser drains",
	"[work.race]") {
	auto [winner, winner_src] = root::make_task_source<int>();
	auto [loser, loser_src] = root::make_task_source<int>();
	auto loser_control = loser.control();

	auto raced = race::race<int>(
		race::race_options{
			.cleanup = race::loser_cleanup_policy::fail_after_cleanup_deadline,
			.loser_cleanup_budget = std::chrono::milliseconds{50},
		},
		race::candidate("winner", std::move(winner)),
		race::candidate("loser", std::move(loser)));

	REQUIRE(winner_src.try_set_value(root::Success<int>{19}));
	CHECK(loser_control.cancel_requested());
	REQUIRE(loser_src.try_set_cancelled(root::CancelReason::requested));

	auto out = root::value(std::move(raced));
	REQUIRE(out.outcome.is_success());
	CHECK(out.outcome.success().value == 19);
}

TEST_CASE(
	"work.race: unsupported cleanup policies fail setup",
	"[work.race]") {
	auto [work, work_src] = root::make_task_source<int>();
	auto control = work.control();

	auto raced = race::race<int>(
		race::race_options{
			.cleanup = race::loser_cleanup_policy::detach_after_cleanup_deadline,
			.loser_cleanup_budget = std::chrono::milliseconds{1},
		},
		race::candidate("work", std::move(work)));

	CHECK(control.cancel_requested());
	try {
		(void)root::value(std::move(raced));
		FAIL("unsupported cleanup policy should fail setup");
	} catch (root::FailureError const &err) { CHECK_THROWS_AS(err.rethrow_cause(), race::race_setup_error); }

	REQUIRE(work_src.try_set_cancelled(root::CancelReason::requested));
}

TEST_CASE(
	"work.race: owned label wrapper keeps dynamic winner label alive",
	"[work.race]") {
	auto [left, left_src] = root::make_task_source<int>();
	auto [right, right_src] = root::make_task_source<int>();
	std::string dynamic = "right";

	auto raced = race::race_owned_labels<int>(
		race::race_options{},
		race::candidate("left", std::move(left)),
		race::candidate(std::string_view{dynamic}, std::move(right)));

	dynamic = "changed";
	REQUIRE(right_src.try_set_value(root::Success<int>{12}));
	REQUIRE(left_src.try_set_value(root::Success<int>{1}));

	auto out = root::value(std::move(raced));
	CHECK(out.winner_label() == "right");
	CHECK(out.result.winner.label == "right");
}

TEST_CASE(
	"work.race: owned label wrapper keeps aggregate failure labels alive",
	"[work.race]") {
	auto [left, left_src] = root::make_task_source<int>();
	auto [right, right_src] = root::make_task_source<int>();
	std::string left_label = "left";
	std::string right_label = "right";

	auto raced = race::race_owned_labels<int>(
		race::race_options{.winner = race::winner_policy::first_success},
		race::candidate(std::string_view{left_label}, std::move(left)),
		race::candidate(std::string_view{right_label}, std::move(right)));

	left_label = "changed-left";
	right_label = "changed-right";
	REQUIRE(left_src.try_set_exception(std::make_exception_ptr(std::runtime_error{"left failed"})));
	REQUIRE(right_src.try_set_exception(std::make_exception_ptr(std::runtime_error{"right failed"})));

	auto out = root::value(std::move(raced));
	REQUIRE(out.result.outcome.is_failure());
	CHECK(out.result.observation.participant_count == 2);
	CHECK(out.result.observation.all_failed);
	try {
		std::rethrow_exception(out.result.outcome.failure().error);
		FAIL("aggregate failure expected");
	} catch (race::owned_race_aggregate_error const &err) {
		REQUIRE(err.entries().size() == 2);
		CHECK(err.entries()[0].label == "left");
		CHECK(err.entries()[1].label == "right");
	}
}
