// Plain TU — not a module unit.
#include<catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier;

namespace root=conflux::work::root;
namespace carrier=conflux::work::carrier;
namespace{
struct OwnerCap{};
struct DriverCap{};
}// namespace
namespace conflux::work::root{
template<>inline constexpr bool enable_address_capability_v<OwnerCap> =true;
template<>inline constexpr bool enable_address_capability_v<DriverCap> =true;
}
// ---------------------------------------------------------------------------
// hop_to_posted — kind + binding
// ---------------------------------------------------------------------------

TEST_CASE(
"phase4: hop_to_posted sets kind to posted and records capability",
"[phase4]"){
OwnerCap owner{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{1}));

auto chain=carrier::from_task(move(task));
CHECK(chain.bound_capability().address==nullptr);

auto hopped=carrier::hop_to_posted(owner,move(chain));
CHECK(hopped.kind()==carrier::CarrierKind::posted);
CHECK(hopped.bound_capability()==root::capability_id(owner));
}
// ---------------------------------------------------------------------------
// hop_to_operation — kind + binding
// ---------------------------------------------------------------------------

TEST_CASE(
"phase4: hop_to_operation sets kind to operation and records capability",
"[phase4]"){
DriverCap driver{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{2}));

auto chain=carrier::from_task(move(task));
auto hopped=carrier::hop_to_operation(driver,move(chain));
CHECK(hopped.kind()==carrier::CarrierKind::operation);
CHECK(hopped.bound_capability()==root::capability_id(driver));
}
// ---------------------------------------------------------------------------
// verify_hop correctness matrix
// ---------------------------------------------------------------------------

TEST_CASE(
"phase4: verify_hop passes for unbound chain with any capability",
"[phase4]"){
OwnerCap const owner{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{3}));

auto chain=carrier::from_task(move(task));
CHECK_NOTHROW(carrier::verify_hop(owner,chain));
}
TEST_CASE(
"phase4: verify_hop passes for bound chain with matching capability",
"[phase4]"){
OwnerCap owner{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{4}));

auto chain=carrier::hop_to_posted(owner,carrier::from_task(move(task)));
CHECK_NOTHROW(carrier::verify_hop(owner,chain));
}
TEST_CASE(
"phase4: verify_hop throws HopCapabilityError for bound chain with wrong capability",
"[phase4]"){
OwnerCap owner_a{};
OwnerCap const owner_b{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{5}));

auto chain=carrier::hop_to_posted(owner_a,carrier::from_task(move(task)));
CHECK_THROWS_AS(carrier::verify_hop(owner_b,chain),carrier::HopCapabilityError);
}
TEST_CASE(
"phase4: HopCapabilityError is catchable as root::JoinError",
"[phase4]"){
OwnerCap owner_a{};
OwnerCap const owner_b{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{6}));

auto chain=carrier::hop_to_posted(owner_a,carrier::from_task(move(task)));
CHECK_THROWS_AS(carrier::verify_hop(owner_b,chain),root::JoinError);
}
// ---------------------------------------------------------------------------
// Rebind
// ---------------------------------------------------------------------------

TEST_CASE(
"phase4: rebind via second hop_to replaces prior binding",
"[phase4]"){
OwnerCap owner_a{};
OwnerCap owner_b{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{7}));

auto c0=carrier::from_task(move(task));
auto c1=carrier::hop_to_posted(owner_a,move(c0));
CHECK(c1.bound_capability()==root::capability_id(owner_a));

auto c2=carrier::hop_to_posted(owner_b,move(c1));
CHECK(c2.bound_capability()==root::capability_id(owner_b));
CHECK(c2.bound_capability()!=root::capability_id(owner_a));
}
// ---------------------------------------------------------------------------
// map drops binding
// ---------------------------------------------------------------------------

TEST_CASE(
"phase4: map drops bound capability, kind is preserved",
"[phase4]"){
OwnerCap owner{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{8}));

auto hopped=carrier::hop_to_posted(owner,carrier::from_task(move(task)));
CHECK(hopped.bound_capability().address!=nullptr);

auto mapped=carrier::map(move(hopped),[](int x){return x*2;});
CHECK(mapped.kind()==carrier::CarrierKind::posted);
CHECK(mapped.bound_capability().address==nullptr);
}
// ---------------------------------------------------------------------------
// when_all aggregate is unbound
// ---------------------------------------------------------------------------

TEST_CASE(
"phase4: when_all aggregate is always unbound",
"[phase4]"){
OwnerCap owner{};
auto[ta,sa]=root::make_task_source<int>();
auto[tb,sb]=root::make_task_source<int>();
REQUIRE(sa.try_set_value(root::Success<int>{1}));
REQUIRE(sb.try_set_value(root::Success<int>{2}));

auto ca=carrier::hop_to_posted(owner,carrier::from_task(move(ta)));
auto cb=carrier::hop_to_posted(owner,carrier::from_task(move(tb)));
auto combined=carrier::when_all(move(ca),move(cb));

CHECK(combined.bound_capability().address==nullptr);
}
// ---------------------------------------------------------------------------
// race preserves winner's binding
// ---------------------------------------------------------------------------

TEST_CASE(
"phase4: race preserves winning chain's bound capability",
"[phase4]"){
OwnerCap owner{};
auto[ta,sa]=root::make_task_source<int>();
auto[tb,sb]=root::make_task_source<int>();
REQUIRE(sa.try_set_value(root::Success<int>{10}));
REQUIRE(sb.try_set_exception(make_exception_ptr(RE{"lose"})));

auto ca=carrier::hop_to_posted(owner,carrier::from_task(move(ta)));
auto cb=carrier::from_task(move(tb));
auto winner=carrier::race(move(ca),move(cb));

CHECK(winner.bound_capability()==root::capability_id(owner));
}
TEST_CASE(
"phase4: race with unbound winner produces unbound result",
"[phase4]"){
OwnerCap owner{};
auto[ta,sa]=root::make_task_source<int>();
auto[tb,sb]=root::make_task_source<int>();
REQUIRE(sa.try_set_exception(make_exception_ptr(RE{"lose"})));
REQUIRE(sb.try_set_value(root::Success<int>{20}));

auto ca=carrier::hop_to_posted(owner,carrier::from_task(move(ta)));
auto cb=carrier::from_task(move(tb));
auto winner=carrier::race(move(ca),move(cb));

CHECK(winner.bound_capability().address==nullptr);
}
