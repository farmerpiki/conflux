// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier.model_a;

namespace root = conflux::work::root;
namespace model_a = conflux::work::carrier::model_a;

namespace {

struct OwnerCap {};
struct DriverCap {};

} // namespace

namespace conflux::work::root {
template<> inline constexpr bool enable_address_capability_v<OwnerCap> = true;
template<> inline constexpr bool enable_address_capability_v<DriverCap> = true;
}

// ---------------------------------------------------------------------------
// hop_to_posted — kind + binding
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase4: hop_to_posted sets kind to posted and records capability",
	"[phase4]") {
	OwnerCap owner{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{1}));

	auto chain = model_a::from_task(std::move(task));
	CHECK(chain.bound_capability().address == nullptr);

	auto hopped = model_a::hop_to_posted(owner, std::move(chain));
	CHECK(hopped.kind() == model_a::CarrierKind::posted);
	CHECK(hopped.bound_capability() == root::capability_id(owner));
}

// ---------------------------------------------------------------------------
// hop_to_operation — kind + binding
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase4: hop_to_operation sets kind to operation and records capability",
	"[phase4]") {
	DriverCap driver{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{2}));

	auto chain = model_a::from_task(std::move(task));
	auto hopped = model_a::hop_to_operation(driver, std::move(chain));
	CHECK(hopped.kind() == model_a::CarrierKind::operation);
	CHECK(hopped.bound_capability() == root::capability_id(driver));
}

// ---------------------------------------------------------------------------
// verify_hop correctness matrix
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase4: verify_hop passes for unbound chain with any capability",
	"[phase4]") {
	OwnerCap const owner{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{3}));

	auto chain = model_a::from_task(std::move(task));
	CHECK_NOTHROW(model_a::verify_hop(owner, chain));
}

TEST_CASE(
	"phase4: verify_hop passes for bound chain with matching capability",
	"[phase4]") {
	OwnerCap owner{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{4}));

	auto chain = model_a::hop_to_posted(owner, model_a::from_task(std::move(task)));
	CHECK_NOTHROW(model_a::verify_hop(owner, chain));
}

TEST_CASE(
	"phase4: verify_hop throws HopCapabilityError for bound chain with wrong capability",
	"[phase4]") {
	OwnerCap owner_a{};
	OwnerCap const owner_b{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{5}));

	auto chain = model_a::hop_to_posted(owner_a, model_a::from_task(std::move(task)));
	CHECK_THROWS_AS(model_a::verify_hop(owner_b, chain), model_a::HopCapabilityError);
}

TEST_CASE(
	"phase4: HopCapabilityError is catchable as root::JoinError",
	"[phase4]") {
	OwnerCap owner_a{};
	OwnerCap const owner_b{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{6}));

	auto chain = model_a::hop_to_posted(owner_a, model_a::from_task(std::move(task)));
	CHECK_THROWS_AS(model_a::verify_hop(owner_b, chain), root::JoinError);
}

// ---------------------------------------------------------------------------
// Rebind
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase4: rebind via second hop_to replaces prior binding",
	"[phase4]") {
	OwnerCap owner_a{};
	OwnerCap owner_b{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{7}));

	auto c0 = model_a::from_task(std::move(task));
	auto c1 = model_a::hop_to_posted(owner_a, std::move(c0));
	CHECK(c1.bound_capability() == root::capability_id(owner_a));

	auto c2 = model_a::hop_to_posted(owner_b, std::move(c1));
	CHECK(c2.bound_capability() == root::capability_id(owner_b));
	CHECK(c2.bound_capability() != root::capability_id(owner_a));
}

// ---------------------------------------------------------------------------
// map drops binding
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase4: map drops bound capability, kind is preserved",
	"[phase4]") {
	OwnerCap owner{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{8}));

	auto hopped = model_a::hop_to_posted(owner, model_a::from_task(std::move(task)));
	CHECK(hopped.bound_capability().address != nullptr);

	auto mapped = model_a::map(std::move(hopped), [](int x) { return x * 2; });
	CHECK(mapped.kind() == model_a::CarrierKind::posted);
	CHECK(mapped.bound_capability().address == nullptr);
}

// ---------------------------------------------------------------------------
// when_all aggregate is unbound
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase4: when_all aggregate is always unbound",
	"[phase4]") {
	OwnerCap owner{};
	auto [ta, sa] = root::make_task_source<int>();
	auto [tb, sb] = root::make_task_source<int>();
	REQUIRE(sa.try_set_value(root::Success<int>{1}));
	REQUIRE(sb.try_set_value(root::Success<int>{2}));

	auto ca = model_a::hop_to_posted(owner, model_a::from_task(std::move(ta)));
	auto cb = model_a::hop_to_posted(owner, model_a::from_task(std::move(tb)));
	auto combined = model_a::when_all(std::move(ca), std::move(cb));

	CHECK(combined.bound_capability().address == nullptr);
}

// ---------------------------------------------------------------------------
// race preserves winner's binding
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase4: race preserves winning chain's bound capability",
	"[phase4]") {
	OwnerCap owner{};
	auto [ta, sa] = root::make_task_source<int>();
	auto [tb, sb] = root::make_task_source<int>();
	REQUIRE(sa.try_set_value(root::Success<int>{10}));
	REQUIRE(sb.try_set_exception(std::make_exception_ptr(std::runtime_error{"lose"})));

	auto ca = model_a::hop_to_posted(owner, model_a::from_task(std::move(ta)));
	auto cb = model_a::from_task(std::move(tb));
	auto winner = model_a::race(std::move(ca), std::move(cb));

	CHECK(winner.bound_capability() == root::capability_id(owner));
}

TEST_CASE(
	"phase4: race with unbound winner produces unbound result",
	"[phase4]") {
	OwnerCap owner{};
	auto [ta, sa] = root::make_task_source<int>();
	auto [tb, sb] = root::make_task_source<int>();
	REQUIRE(sa.try_set_exception(std::make_exception_ptr(std::runtime_error{"lose"})));
	REQUIRE(sb.try_set_value(root::Success<int>{20}));

	auto ca = model_a::hop_to_posted(owner, model_a::from_task(std::move(ta)));
	auto cb = model_a::from_task(std::move(tb));
	auto winner = model_a::race(std::move(ca), std::move(cb));

	CHECK(winner.bound_capability().address == nullptr);
}
