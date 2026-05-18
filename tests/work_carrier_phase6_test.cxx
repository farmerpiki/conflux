// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier;
import conflux.work.carrier.scope;

namespace root = conflux::work::root;
namespace carrier = conflux::work::carrier;
namespace carrier = conflux::work::carrier;
namespace {

struct OwnerCap {};
struct DriverCap {};
carrier::Chain<int> make_success(
	int v) {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{v});
	return carrier::from_task(move(task));
}
carrier::Chain<int> make_failure(
	std::string_view msg = "fail") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_exception(make_exception_ptr(std::runtime_error{std::string{msg}}));
	return carrier::from_task(move(task));
}
carrier::Chain<int> make_cancelled() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_cancelled(root::work_errc::cancelled_requested);
	return carrier::from_task(move(task));
}

} // namespace
namespace conflux::work::root {

template<>
inline constexpr bool enable_address_capability_v<OwnerCap> = true;
template<>
inline constexpr bool enable_address_capability_v<DriverCap> = true;

} // namespace conflux::work::root
// ---------------------------------------------------------------------------
// Phase 6a: hop_to_task + unbind
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase6a: hop_to_task resets kind to task and clears bound_cap",
	"[phase6a]") {
	OwnerCap owner{};
	auto chain = carrier::hop_to_posted(owner, make_success(1));
	REQUIRE(chain.kind() == carrier::CarrierKind::posted);
	REQUIRE(chain.bound_capability() == root::capability_id(owner));

	auto task_chain = carrier::hop_to_task(move(chain));
	CHECK(task_chain.kind() == carrier::CarrierKind::task);
	CHECK(task_chain.bound_capability().address == nullptr);
}
TEST_CASE(
	"phase6a: hop_to_task preserves outcome",
	"[phase6a]") {
	auto chain = make_success(42);
	auto task_chain = carrier::hop_to_task(move(chain));
	auto out = move(task_chain).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 42);
}
TEST_CASE(
	"phase6a: hop_to_task on already-task chain is a no-op",
	"[phase6a]") {
	auto chain = make_success(7);
	REQUIRE(chain.kind() == carrier::CarrierKind::task);
	auto chain2 = carrier::hop_to_task(move(chain));
	CHECK(chain2.kind() == carrier::CarrierKind::task);
	CHECK(chain2.bound_capability().address == nullptr);
}
TEST_CASE(
	"phase6a: unbind clears bound_cap without changing kind",
	"[phase6a]") {
	OwnerCap owner{};
	auto chain = carrier::hop_to_posted(owner, make_success(5));
	REQUIRE(chain.kind() == carrier::CarrierKind::posted);
	REQUIRE(chain.bound_capability() == root::capability_id(owner));

	auto unbound = carrier::unbind(move(chain));
	CHECK(unbound.kind() == carrier::CarrierKind::posted);
	CHECK(unbound.bound_capability().address == nullptr);
}
TEST_CASE(
	"phase6a: unbind on unbound task chain is a no-op",
	"[phase6a]") {
	auto chain = make_success(3);
	auto unbound = carrier::unbind(move(chain));
	CHECK(unbound.kind() == carrier::CarrierKind::task);
	CHECK(unbound.bound_capability().address == nullptr);
}
// ---------------------------------------------------------------------------
// Phase 6b: into_ready_task
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase6b: into_ready_task success branch produces equivalent Task",
	"[phase6b]") {
	auto task = carrier::into_ready_task(make_success(99));
	auto out = root::blocking_join(move(task));
	REQUIRE(out.is_success());
	CHECK(out.success().value == 99);
}
TEST_CASE(
	"phase6b: into_ready_task failure branch produces equivalent Task",
	"[phase6b]") {
	auto task = carrier::into_ready_task(make_failure("oops"));
	auto out = root::blocking_join(move(task));
	REQUIRE(out.is_failure());
	CHECK_THROWS_AS(rethrow_exception(out.failure().error), std::runtime_error);
}
TEST_CASE(
	"phase6b: into_ready_task cancelled branch produces equivalent Task",
	"[phase6b]") {
	auto task = carrier::into_ready_task(make_cancelled());
	auto out = root::blocking_join(move(task));
	REQUIRE(out.is_cancelled());
	CHECK(out.cancelled().reason == root::CancelReason::requested);
}
TEST_CASE(
	"phase6b: into_ready_task on bound chain drops bound_cap",
	"[phase6b]") {
	OwnerCap owner{};
	auto chain = carrier::hop_to_posted(owner, make_success(1));
	REQUIRE(chain.bound_capability().address != nullptr);

	auto task = carrier::into_ready_task(move(chain));
	auto out = root::blocking_join(move(task));
	CHECK(out.is_success());
}
// ---------------------------------------------------------------------------
// Phase 6c: when_all multi-failure aggregation
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase6c: when_all dual failure produces AggregateError",
	"[phase6c]") {
	auto a = make_failure("err-a");
	auto b = make_failure("err-b");
	auto combined = carrier::when_all(move(a), move(b));
	auto out = move(combined).release_outcome();
	REQUIRE(out.is_failure());
	auto agg_ep = out.failure().error;
	CHECK_THROWS_AS(rethrow_exception(agg_ep), carrier::AggregateError);
}
TEST_CASE(
	"phase6c: AggregateError contains both causes",
	"[phase6c]") {
	auto a = make_failure("err-a");
	auto b = make_failure("err-b");
	auto combined = carrier::when_all(move(a), move(b));
	auto out = move(combined).release_outcome();
	REQUIRE(out.is_failure());

	try {
		rethrow_exception(out.failure().error);
	} catch (carrier::AggregateError const &ae) {
		auto causes = ae.causes_owned();
		REQUIRE(causes.size() == 2);
		CHECK(causes[0] != nullptr);
		CHECK(causes[1] != nullptr);
	}
}
TEST_CASE(
	"phase6c: AggregateError is catchable as WorkError",
	"[phase6c]") {
	auto combined = carrier::when_all(make_failure(), make_failure());
	auto out = move(combined).release_outcome();
	REQUIRE(out.is_failure());
	CHECK_THROWS_AS(rethrow_exception(out.failure().error), root::WorkError);
}
TEST_CASE(
	"phase6c: when_all single A failure returns original cause unwrapped",
	"[phase6c]") {
	auto combined = carrier::when_all(make_failure("sole"), make_success(1));
	auto out = move(combined).release_outcome();
	REQUIRE(out.is_failure());
	try {
		rethrow_exception(out.failure().error);
	} catch (carrier::AggregateError const &) {
		FAIL("single failure must not produce AggregateError");
	} catch (std::runtime_error const &) { CHECK(true); }
}
TEST_CASE(
	"phase6c: when_all single B failure returns original cause unwrapped",
	"[phase6c]") {
	auto combined = carrier::when_all(make_success(1), make_failure("sole"));
	auto out = move(combined).release_outcome();
	REQUIRE(out.is_failure());
	try {
		rethrow_exception(out.failure().error);
	} catch (carrier::AggregateError const &) {
		FAIL("single failure must not produce AggregateError");
	} catch (std::runtime_error const &) { CHECK(true); }
}
TEST_CASE(
	"phase6c: when_all A failure B cancelled returns A failure",
	"[phase6c]") {
	auto combined = carrier::when_all(make_failure(), make_cancelled());
	auto out = move(combined).release_outcome();
	CHECK(out.is_failure());
}
TEST_CASE(
	"phase6c: when_all A cancelled B failure returns B failure",
	"[phase6c]") {
	auto combined = carrier::when_all(make_cancelled(), make_failure());
	auto out = move(combined).release_outcome();
	CHECK(out.is_failure());
}
// ---------------------------------------------------------------------------
// Phase 6d: HopCapabilityError reparented to JoinError
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase6d: HopCapabilityError catchable as JoinError",
	"[phase6d]") {
	OwnerCap owner_a{};
	OwnerCap const owner_b{};
	auto chain = carrier::hop_to_posted(owner_a, make_success(1));
	CHECK_THROWS_AS(carrier::verify_hop(owner_b, chain), root::JoinError);
}
TEST_CASE(
	"phase6d: HopCapabilityError reason is hop_capability_mismatch",
	"[phase6d]") {
	OwnerCap owner_a{};
	OwnerCap const owner_b{};
	auto chain = carrier::hop_to_posted(owner_a, make_success(1));
	try {
		carrier::verify_hop(owner_b, chain);
		FAIL("expected HopCapabilityError");
	} catch (root::JoinError const &e) { CHECK(e.reason_code() == root::JoinError::reason::hop_capability_mismatch); }
}
TEST_CASE(
	"phase6d: HopCapabilityError still catchable as JoinError",
	"[phase6d]") {
	OwnerCap owner_a{};
	OwnerCap const owner_b{};
	auto chain = carrier::hop_to_posted(owner_a, make_success(1));
	CHECK_THROWS_AS(carrier::verify_hop(owner_b, chain), root::JoinError);
}
// ---------------------------------------------------------------------------
// Phase 6e: Scope::admit auto-bind + admit_unbound
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase6e: Scope::admit PostedJoinHandle auto-binds capability",
	"[phase6e]") {
	OwnerCap owner{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	auto jh = root::into_join_handle(move(posted));
	REQUIRE(src.try_set_value(root::Success<int>{10}));

	carrier::Scope scope{};
	auto chain = scope.admit(owner, move(jh));
	CHECK(chain.kind() == carrier::CarrierKind::posted);
	CHECK(chain.bound_capability() == root::capability_id(owner));
}
TEST_CASE(
	"phase6e: Scope::admit_unbound PostedJoinHandle leaves cap null",
	"[phase6e]") {
	OwnerCap owner{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	auto jh = root::into_join_handle(move(posted));
	REQUIRE(src.try_set_value(root::Success<int>{10}));

	carrier::Scope scope{};
	auto chain = scope.admit_unbound(owner, move(jh));
	CHECK(chain.kind() == carrier::CarrierKind::posted);
	CHECK(chain.bound_capability().address == nullptr);
}
TEST_CASE(
	"phase6e: Scope::admit auto-bind causes verify_hop to throw for wrong cap",
	"[phase6e]") {
	OwnerCap owner_a{};
	OwnerCap const owner_b{};
	auto [posted, src] = root::make_posted_source<int>(owner_a);
	auto jh = root::into_join_handle(move(posted));
	REQUIRE(src.try_set_value(root::Success<int>{0}));

	carrier::Scope scope{};
	auto chain = scope.admit(owner_a, move(jh));
	CHECK_THROWS_AS(carrier::verify_hop(owner_b, chain), carrier::HopCapabilityError);
}
TEST_CASE(
	"phase6e: Scope::admit OperationJoinHandle auto-binds capability",
	"[phase6e]") {
	DriverCap driver{};
	auto [op, src] = root::make_operation_source<int>(driver);
	auto jh = root::into_join_handle(move(op));
	REQUIRE(src.try_set_value(root::Success<int>{20}));

	carrier::Scope scope{};
	auto chain = scope.admit(driver, move(jh));
	CHECK(chain.kind() == carrier::CarrierKind::operation);
	CHECK(chain.bound_capability() == root::capability_id(driver));
}
TEST_CASE(
	"phase6e: Scope::admit_unbound OperationJoinHandle leaves cap null",
	"[phase6e]") {
	DriverCap driver{};
	auto [op, src] = root::make_operation_source<int>(driver);
	auto jh = root::into_join_handle(move(op));
	REQUIRE(src.try_set_value(root::Success<int>{20}));

	carrier::Scope scope{};
	auto chain = scope.admit_unbound(driver, move(jh));
	CHECK(chain.kind() == carrier::CarrierKind::operation);
	CHECK(chain.bound_capability().address == nullptr);
}
TEST_CASE(
	"phase6e: Scope::admit TaskJoinHandle remains unbound (no capability to bind)",
	"[phase6e]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	REQUIRE(src.try_set_value(root::Success<int>{5}));

	carrier::Scope scope{};
	auto chain = scope.admit(move(jh));
	CHECK(chain.kind() == carrier::CarrierKind::task);
	CHECK(chain.bound_capability().address == nullptr);
}
