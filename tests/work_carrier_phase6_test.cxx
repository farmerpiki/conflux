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

struct OwnerCap : root::capability_id_from_address {};
struct DriverCap : root::capability_id_from_address {};

model_a::Chain<int> make_success(
	int v) {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{v});
	return model_a::from_task(std::move(task));
}

model_a::Chain<int> make_failure(
	SV msg = "fail") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_exception(std::make_exception_ptr(std::runtime_error{S{msg}}));
	return model_a::from_task(std::move(task));
}

model_a::Chain<int> make_cancelled() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_cancelled(root::CancelReason::requested);
	return model_a::from_task(std::move(task));
}

} // namespace

// ---------------------------------------------------------------------------
// Phase 6a: hop_to_task + unbind
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase6a: hop_to_task resets kind to task and clears bound_cap",
	"[phase6a]") {
	OwnerCap owner{};
	auto chain = model_a::hop_to_posted(owner, make_success(1));
	REQUIRE(chain.kind() == model_a::CarrierKind::posted);
	REQUIRE(chain.bound_capability() == root::capability_id(owner));

	auto task_chain = model_a::hop_to_task(std::move(chain));
	CHECK(task_chain.kind() == model_a::CarrierKind::task);
	CHECK(task_chain.bound_capability().address == nullptr);
}

TEST_CASE(
	"phase6a: hop_to_task preserves outcome",
	"[phase6a]") {
	auto chain = make_success(42);
	auto task_chain = model_a::hop_to_task(std::move(chain));
	auto out = std::move(task_chain).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 42);
}

TEST_CASE(
	"phase6a: hop_to_task on already-task chain is a no-op",
	"[phase6a]") {
	auto chain = make_success(7);
	REQUIRE(chain.kind() == model_a::CarrierKind::task);
	auto chain2 = model_a::hop_to_task(std::move(chain));
	CHECK(chain2.kind() == model_a::CarrierKind::task);
	CHECK(chain2.bound_capability().address == nullptr);
}

TEST_CASE(
	"phase6a: unbind clears bound_cap without changing kind",
	"[phase6a]") {
	OwnerCap owner{};
	auto chain = model_a::hop_to_posted(owner, make_success(5));
	REQUIRE(chain.kind() == model_a::CarrierKind::posted);
	REQUIRE(chain.bound_capability() == root::capability_id(owner));

	auto unbound = model_a::unbind(std::move(chain));
	CHECK(unbound.kind() == model_a::CarrierKind::posted);
	CHECK(unbound.bound_capability().address == nullptr);
}

TEST_CASE(
	"phase6a: unbind on unbound task chain is a no-op",
	"[phase6a]") {
	auto chain = make_success(3);
	auto unbound = model_a::unbind(std::move(chain));
	CHECK(unbound.kind() == model_a::CarrierKind::task);
	CHECK(unbound.bound_capability().address == nullptr);
}

// ---------------------------------------------------------------------------
// Phase 6b: into_ready_task
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase6b: into_ready_task success branch produces equivalent Task",
	"[phase6b]") {
	auto task = model_a::into_ready_task(make_success(99));
	auto out = root::join(std::move(task));
	REQUIRE(out.is_success());
	CHECK(out.success().value == 99);
}

TEST_CASE(
	"phase6b: into_ready_task failure branch produces equivalent Task",
	"[phase6b]") {
	auto task = model_a::into_ready_task(make_failure("oops"));
	auto out = root::join(std::move(task));
	REQUIRE(out.is_failure());
	CHECK_THROWS_AS(std::rethrow_exception(out.failure().error), std::runtime_error);
}

TEST_CASE(
	"phase6b: into_ready_task cancelled branch produces equivalent Task",
	"[phase6b]") {
	auto task = model_a::into_ready_task(make_cancelled());
	auto out = root::join(std::move(task));
	REQUIRE(out.is_cancelled());
	CHECK(out.cancelled().reason == root::CancelReason::requested);
}

TEST_CASE(
	"phase6b: into_ready_task on bound chain drops bound_cap",
	"[phase6b]") {
	OwnerCap owner{};
	auto chain = model_a::hop_to_posted(owner, make_success(1));
	REQUIRE(chain.bound_capability().address != nullptr);

	auto task = model_a::into_ready_task(std::move(chain));
	auto out = root::join(std::move(task));
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
	auto combined = model_a::when_all(std::move(a), std::move(b));
	auto out = std::move(combined).release_outcome();
	REQUIRE(out.is_failure());
	auto agg_ep = out.failure().error;
	CHECK_THROWS_AS(std::rethrow_exception(agg_ep), model_a::AggregateError);
}

TEST_CASE(
	"phase6c: AggregateError contains both causes",
	"[phase6c]") {
	auto a = make_failure("err-a");
	auto b = make_failure("err-b");
	auto combined = model_a::when_all(std::move(a), std::move(b));
	auto out = std::move(combined).release_outcome();
	REQUIRE(out.is_failure());

	try {
		std::rethrow_exception(out.failure().error);
	} catch (model_a::AggregateError const &ae) {
		auto causes = ae.causes_owned();
		REQUIRE(causes.size() == 2);
		CHECK(causes[0] != nullptr);
		CHECK(causes[1] != nullptr);
	}
}

TEST_CASE(
	"phase6c: AggregateError is catchable as WorkError",
	"[phase6c]") {
	auto combined = model_a::when_all(make_failure(), make_failure());
	auto out = std::move(combined).release_outcome();
	REQUIRE(out.is_failure());
	CHECK_THROWS_AS(std::rethrow_exception(out.failure().error), root::WorkError);
}

TEST_CASE(
	"phase6c: when_all single A failure returns original cause unwrapped",
	"[phase6c]") {
	auto combined = model_a::when_all(make_failure("sole"), make_success(1));
	auto out = std::move(combined).release_outcome();
	REQUIRE(out.is_failure());
	try {
		std::rethrow_exception(out.failure().error);
	} catch (model_a::AggregateError const &) {
		FAIL("single failure must not produce AggregateError");
	} catch (std::runtime_error const &) { CHECK(true); }
}

TEST_CASE(
	"phase6c: when_all single B failure returns original cause unwrapped",
	"[phase6c]") {
	auto combined = model_a::when_all(make_success(1), make_failure("sole"));
	auto out = std::move(combined).release_outcome();
	REQUIRE(out.is_failure());
	try {
		std::rethrow_exception(out.failure().error);
	} catch (model_a::AggregateError const &) {
		FAIL("single failure must not produce AggregateError");
	} catch (std::runtime_error const &) { CHECK(true); }
}

TEST_CASE(
	"phase6c: when_all A failure B cancelled returns A failure",
	"[phase6c]") {
	auto combined = model_a::when_all(make_failure(), make_cancelled());
	auto out = std::move(combined).release_outcome();
	CHECK(out.is_failure());
}

TEST_CASE(
	"phase6c: when_all A cancelled B failure returns B failure",
	"[phase6c]") {
	auto combined = model_a::when_all(make_cancelled(), make_failure());
	auto out = std::move(combined).release_outcome();
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
	auto chain = model_a::hop_to_posted(owner_a, make_success(1));
	CHECK_THROWS_AS(model_a::verify_hop(owner_b, chain), root::JoinError);
}

TEST_CASE(
	"phase6d: HopCapabilityError reason is hop_capability_mismatch",
	"[phase6d]") {
	OwnerCap owner_a{};
	OwnerCap const owner_b{};
	auto chain = model_a::hop_to_posted(owner_a, make_success(1));
	try {
		model_a::verify_hop(owner_b, chain);
		FAIL("expected HopCapabilityError");
	} catch (root::JoinError const &e) { CHECK(e.reason_code() == root::JoinError::reason::hop_capability_mismatch); }
}

TEST_CASE(
	"phase6d: HopCapabilityError still catchable as JoinError",
	"[phase6d]") {
	OwnerCap owner_a{};
	OwnerCap const owner_b{};
	auto chain = model_a::hop_to_posted(owner_a, make_success(1));
	CHECK_THROWS_AS(model_a::verify_hop(owner_b, chain), root::JoinError);
}

// ---------------------------------------------------------------------------
// Phase 6e: Scope::admit auto-bind + admit_unbound
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase6e: Scope::admit PostedJoinHandle auto-binds capability",
	"[phase6e]") {
	OwnerCap owner{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	auto jh = root::into_join_handle(std::move(posted));
	REQUIRE(src.try_set_value(root::Success<int>{10}));

	carrier::Scope scope{};
	auto chain = scope.admit(owner, std::move(jh));
	CHECK(chain.kind() == model_a::CarrierKind::posted);
	CHECK(chain.bound_capability() == root::capability_id(owner));
}

TEST_CASE(
	"phase6e: Scope::admit_unbound PostedJoinHandle leaves cap null",
	"[phase6e]") {
	OwnerCap owner{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	auto jh = root::into_join_handle(std::move(posted));
	REQUIRE(src.try_set_value(root::Success<int>{10}));

	carrier::Scope scope{};
	auto chain = scope.admit_unbound(owner, std::move(jh));
	CHECK(chain.kind() == model_a::CarrierKind::posted);
	CHECK(chain.bound_capability().address == nullptr);
}

TEST_CASE(
	"phase6e: Scope::admit auto-bind causes verify_hop to throw for wrong cap",
	"[phase6e]") {
	OwnerCap owner_a{};
	OwnerCap const owner_b{};
	auto [posted, src] = root::make_posted_source<int>(owner_a);
	auto jh = root::into_join_handle(std::move(posted));
	REQUIRE(src.try_set_value(root::Success<int>{0}));

	carrier::Scope scope{};
	auto chain = scope.admit(owner_a, std::move(jh));
	CHECK_THROWS_AS(model_a::verify_hop(owner_b, chain), model_a::HopCapabilityError);
}

TEST_CASE(
	"phase6e: Scope::admit OperationJoinHandle auto-binds capability",
	"[phase6e]") {
	DriverCap driver{};
	auto [op, src] = root::make_operation_source<int>(driver);
	auto jh = root::into_join_handle(std::move(op));
	REQUIRE(src.try_set_value(root::Success<int>{20}));

	carrier::Scope scope{};
	auto chain = scope.admit(driver, std::move(jh));
	CHECK(chain.kind() == model_a::CarrierKind::operation);
	CHECK(chain.bound_capability() == root::capability_id(driver));
}

TEST_CASE(
	"phase6e: Scope::admit_unbound OperationJoinHandle leaves cap null",
	"[phase6e]") {
	DriverCap driver{};
	auto [op, src] = root::make_operation_source<int>(driver);
	auto jh = root::into_join_handle(std::move(op));
	REQUIRE(src.try_set_value(root::Success<int>{20}));

	carrier::Scope scope{};
	auto chain = scope.admit_unbound(driver, std::move(jh));
	CHECK(chain.kind() == model_a::CarrierKind::operation);
	CHECK(chain.bound_capability().address == nullptr);
}

TEST_CASE(
	"phase6e: Scope::admit TaskJoinHandle remains unbound (no capability to bind)",
	"[phase6e]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(std::move(task));
	REQUIRE(src.try_set_value(root::Success<int>{5}));

	carrier::Scope scope{};
	auto chain = scope.admit(std::move(jh));
	CHECK(chain.kind() == model_a::CarrierKind::task);
	CHECK(chain.bound_capability().address == nullptr);
}
