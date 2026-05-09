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
// Model A
// ---------------------------------------------------------------------------

TEST_CASE(
"carrier.model_a: from_task preserves success outcome and task kind",
"[carrier.model_a]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{42}));

auto chain=carrier::from_task(move(task));
CHECK(chain.kind()==carrier::CarrierKind::task);

auto out=move(chain).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==42);
}
TEST_CASE(
"carrier.model_a: from_task preserves failure outcome",
"[carrier.model_a]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_exception(make_exception_ptr(RE{"fail"})));

auto chain=carrier::from_task(move(task));
auto out=move(chain).release_outcome();
REQUIRE(out.is_failure());
}
TEST_CASE(
"carrier.model_a: from_task preserves cancelled outcome",
"[carrier.model_a]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));

auto chain=carrier::from_task(move(task));
auto out=move(chain).release_outcome();
REQUIRE(out.is_cancelled());
CHECK(out.cancelled().reason==root::CancelReason::requested);
}
TEST_CASE(
"carrier.model_a: from_posted preserves outcome and posted kind",
"[carrier.model_a]"){
OwnerCap owner{};
auto[posted,src]=root::make_posted_source<int>(owner);
REQUIRE(src.try_set_value(root::Success<int>{7}));

auto chain=carrier::from_posted(owner,move(posted));
CHECK(chain.kind()==carrier::CarrierKind::posted);

auto out=move(chain).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==7);
}
TEST_CASE(
"carrier.model_a: from_operation preserves outcome and operation kind",
"[carrier.model_a]"){
DriverCap driver{};
auto[op,src]=root::make_operation_source<int>(driver);
REQUIRE(src.try_set_value(root::Success<int>{5}));

auto chain=carrier::from_operation(driver,move(op));
CHECK(chain.kind()==carrier::CarrierKind::operation);

auto out=move(chain).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==5);
}
TEST_CASE(
"carrier.model_a: map transforms success value and preserves kind",
"[carrier.model_a]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{10}));

auto chain=carrier::from_task(move(task));
auto mapped=carrier::map(move(chain),[](int x){return x*3;});

CHECK(mapped.kind()==carrier::CarrierKind::task);
auto out=move(mapped).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==30);
}
TEST_CASE(
"carrier.model_a: map passes through failure without calling fn",
"[carrier.model_a]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_exception(make_exception_ptr(RE{"boom"})));

bool fn_called=false;
auto chain=carrier::from_task(move(task));
auto mapped=carrier::map(move(chain),[&fn_called](int x){
fn_called=true;
return x;
});

CHECK_FALSE(fn_called);
auto out=move(mapped).release_outcome();
CHECK(out.is_failure());
}
TEST_CASE(
"carrier.model_a: map passes through cancelled without calling fn",
"[carrier.model_a]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_shutdown));

bool fn_called=false;
auto chain=carrier::from_task(move(task));
auto mapped=carrier::map(move(chain),[&fn_called](int x){
fn_called=true;
return x;
});

CHECK_FALSE(fn_called);
auto out=move(mapped).release_outcome();
REQUIRE(out.is_cancelled());
CHECK(out.cancelled().reason==root::CancelReason::shutdown);
}
TEST_CASE(
"carrier.model_a: map wraps throwing fn as failure",
"[carrier.model_a]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{1}));

auto chain=carrier::from_task(move(task));
auto mapped=carrier::map(move(chain),[](int)->int{throw RE{"fn threw"};});

auto out=move(mapped).release_outcome();
REQUIRE(out.is_failure());
CHECK_THROWS_AS(rethrow_exception(out.failure().error),RE);
}
TEST_CASE(
"carrier.model_a: then delegates to map",
"[carrier.model_a]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{4}));

auto chain=carrier::from_task(move(task));
auto result=carrier::then(move(chain),[](int x){return x+6;});

auto out=move(result).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==10);
}
TEST_CASE(
"carrier.model_a: map chain 3 stages",
"[carrier.model_a]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{1}));

auto c0=carrier::from_task(move(task));
auto c1=carrier::map(move(c0),[](int x){return x+1;});
auto c2=carrier::map(move(c1),[](int x){return x*10;});
auto c3=carrier::map(move(c2),[](int x){return x-5;});

auto out=move(c3).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==15);
}
TEST_CASE(
"carrier.model_a: when_all both success produces Tup",
"[carrier.model_a]"){
auto[task_a,src_a]=root::make_task_source<int>();
auto[task_b,src_b]=root::make_task_source<int>();
REQUIRE(src_a.try_set_value(root::Success<int>{3}));
REQUIRE(src_b.try_set_value(root::Success<int>{7}));

auto ca=carrier::from_task(move(task_a));
auto cb=carrier::from_task(move(task_b));
auto combined=carrier::when_all(move(ca),move(cb));

CHECK(combined.kind()==carrier::CarrierKind::task);
auto out=move(combined).release_outcome();
REQUIRE(out.is_success());
CHECK(std::get<0>(out.success().value)==3);
CHECK(std::get<1>(out.success().value)==7);
}
TEST_CASE(
"carrier.model_a: when_all first failure takes priority",
"[carrier.model_a]"){
auto[task_a,src_a]=root::make_task_source<int>();
auto[task_b,src_b]=root::make_task_source<int>();
REQUIRE(src_a.try_set_exception(make_exception_ptr(RE{"a"})));
REQUIRE(src_b.try_set_value(root::Success<int>{1}));

auto ca=carrier::from_task(move(task_a));
auto cb=carrier::from_task(move(task_b));
auto combined=carrier::when_all(move(ca),move(cb));

auto out=move(combined).release_outcome();
CHECK(out.is_failure());
}
TEST_CASE(
"carrier.model_a: when_all second failure takes priority over success",
"[carrier.model_a]"){
auto[task_a,src_a]=root::make_task_source<int>();
auto[task_b,src_b]=root::make_task_source<int>();
REQUIRE(src_a.try_set_value(root::Success<int>{1}));
REQUIRE(src_b.try_set_exception(make_exception_ptr(RE{"b"})));

auto ca=carrier::from_task(move(task_a));
auto cb=carrier::from_task(move(task_b));
auto combined=carrier::when_all(move(ca),move(cb));

auto out=move(combined).release_outcome();
CHECK(out.is_failure());
}
TEST_CASE(
"carrier.model_a: when_all first cancel (no failure) yields cancelled",
"[carrier.model_a]"){
auto[task_a,src_a]=root::make_task_source<int>();
auto[task_b,src_b]=root::make_task_source<int>();
REQUIRE(src_a.try_set_cancelled(root::work_errc::cancelled_requested));
REQUIRE(src_b.try_set_value(root::Success<int>{1}));

auto ca=carrier::from_task(move(task_a));
auto cb=carrier::from_task(move(task_b));
auto combined=carrier::when_all(move(ca),move(cb));

auto out=move(combined).release_outcome();
CHECK(out.is_cancelled());
}
TEST_CASE(
"carrier.model_a: hop_to_posted changes kind to posted",
"[carrier.model_a]"){
OwnerCap owner{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{9}));

auto chain=carrier::from_task(move(task));
CHECK(chain.kind()==carrier::CarrierKind::task);

auto hopped=carrier::hop_to_posted(owner,move(chain));
CHECK(hopped.kind()==carrier::CarrierKind::posted);

auto out=move(hopped).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==9);
}
TEST_CASE(
"carrier.model_a: hop_to_operation changes kind to operation",
"[carrier.model_a]"){
DriverCap driver{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{11}));

auto chain=carrier::from_task(move(task));
auto hopped=carrier::hop_to_operation(driver,move(chain));
CHECK(hopped.kind()==carrier::CarrierKind::operation);

auto out=move(hopped).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==11);
}
TEST_CASE(
"carrier.model_a: kind preserved through map",
"[carrier.model_a]"){
OwnerCap owner{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{2}));

auto chain=carrier::from_task(move(task));
auto hopped=carrier::hop_to_posted(owner,move(chain));
auto mapped=carrier::map(move(hopped),[](int x){return x*2;});

CHECK(mapped.kind()==carrier::CarrierKind::posted);
auto out=move(mapped).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==4);
}
// ---------------------------------------------------------------------------
// E1.z — Chain member combinators
// ---------------------------------------------------------------------------

TEST_CASE(
"chain.then: transforms success value",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{5}));
auto out=move(carrier::from_task(move(task)).then([](int x){return x*3;})).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==15);
}
TEST_CASE(
"chain.then: passes through failure without calling fn",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_exception(make_exception_ptr(RE{"e"})));
bool called=false;
auto out=move(carrier::from_task(move(task)).then([&](int x){
called=true;
return x;
})).release_outcome();
CHECK_FALSE(called);
CHECK(out.is_failure());
}
TEST_CASE(
"chain.then: passes through cancel without calling fn",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
bool called=false;
auto out=move(carrier::from_task(move(task)).then([&](int x){
called=true;
return x;
})).release_outcome();
CHECK_FALSE(called);
CHECK(out.is_cancelled());
}
TEST_CASE(
"chain.then: fn throw becomes failure",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{1}));
auto out=move(carrier::from_task(move(task)).then([](int)->int{
throw RE{"bad"};
})).release_outcome();
CHECK(out.is_failure());
}
TEST_CASE(
"chain.then: void T success applies fn",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<void>();
REQUIRE(src.try_set_value(root::Success<void>{}));
int side=0;
auto out=move(carrier::from_task(move(task)).then([&]{
side=42;
return side;
})).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==42);
CHECK(side==42);
}
TEST_CASE(
"chain.catch_error: success passes through",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{7}));
bool called=false;
auto out=move(carrier::from_task(move(task)).catch_error([&](EP){
called=true;
return-1;
})).release_outcome();
CHECK_FALSE(called);
REQUIRE(out.is_success());
CHECK(out.success().value==7);
}
TEST_CASE(
"chain.catch_error: failure handled returning T",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_exception(make_exception_ptr(RE{"e"})));
auto out=move(carrier::from_task(move(task)).catch_error([](EP){
return 99;
})).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==99);
}
TEST_CASE(
"chain.catch_error: failure handled returning Chain<T>",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_exception(make_exception_ptr(RE{"e"})));
auto out=move(carrier::from_task(move(task)).catch_error([](EP)->carrier::Chain<int>{
auto[t2,s2]=root::make_task_source<int>();
(void)s2.try_set_value(root::Success<int>{55});
return carrier::from_task(move(t2));
})).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==55);
}
TEST_CASE(
"chain.catch_error: cancel passes through",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
bool called=false;
auto out=move(carrier::from_task(move(task)).catch_error([&](EP){
called=true;
return-1;
})).release_outcome();
CHECK_FALSE(called);
CHECK(out.is_cancelled());
}
TEST_CASE(
"chain.on_cancel: cancel side-effect fires, still cancelled",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
bool called=false;
auto out=move(carrier::from_task(move(task)).on_cancel([&]{called=true;})).release_outcome();
CHECK(called);
CHECK(out.is_cancelled());
}
TEST_CASE(
"chain.on_cancel: success passes through without calling fn",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{3}));
bool called=false;
auto out=move(carrier::from_task(move(task)).on_cancel([&]{called=true;})).release_outcome();
CHECK_FALSE(called);
REQUIRE(out.is_success());
CHECK(out.success().value==3);
}
TEST_CASE(
"chain.on_cancel: throwing fn is swallowed; still cancelled",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
auto out=move(carrier::from_task(move(task)).on_cancel([]{
throw RE{"x"};
})).release_outcome();
CHECK(out.is_cancelled());
}
TEST_CASE(
"chain.recover_cancel: cancel recovery returns T",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
auto out=move(carrier::from_task(move(task)).recover_cancel([]{return 42;})).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==42);
}
TEST_CASE(
"chain.recover_cancel: success passes through",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{8}));
bool called=false;
auto out=move(carrier::from_task(move(task)).recover_cancel([&]{
called=true;
return-1;
})).release_outcome();
CHECK_FALSE(called);
REQUIRE(out.is_success());
CHECK(out.success().value==8);
}
TEST_CASE(
"chain.recover_cancel: failure passes through",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_exception(make_exception_ptr(RE{"e"})));
bool called=false;
auto out=move(carrier::from_task(move(task)).recover_cancel([&]{
called=true;
return-1;
})).release_outcome();
CHECK_FALSE(called);
CHECK(out.is_failure());
}
TEST_CASE(
"chain.recover: failure recovered",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_exception(make_exception_ptr(RE{"e"})));
auto out=
move(carrier::from_task(move(task)).recover([](root::Outcome<int>){return 77;})).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==77);
}
TEST_CASE(
"chain.recover: cancel recovered",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
auto out=
move(carrier::from_task(move(task)).recover([](root::Outcome<int>){return 33;})).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==33);
}
TEST_CASE(
"chain.recover: success passes through",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{6}));
bool called=false;
auto out=move(carrier::from_task(move(task)).recover([&](root::Outcome<int>){
called=true;
return-1;
})).release_outcome();
CHECK_FALSE(called);
REQUIRE(out.is_success());
CHECK(out.success().value==6);
}
TEST_CASE(
"chain.transform_outcome: success → success (identity)",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{4}));
auto out=move(carrier::from_task(move(task)).transform_outcome([](root::Outcome<int>o){
return o;
})).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==4);
}
TEST_CASE(
"chain.transform_outcome: maps success value to different type",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{10}));
auto out=move(
carrier::from_task(move(task))
.transform_outcome([](root::Outcome<int>o)->root::Outcome<S>{
if(o.is_success())
return root::Outcome<S>{
root::Success<S>{to_string(o.success().value)}};
return root::Outcome<S>{move(o).failure()};
}))
.release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value=="10");
}
TEST_CASE(
"chain.schedule_on: binds capability, preserves outcome",
"[chain.combinators]"){
OwnerCap cap{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{9}));
auto chain=carrier::from_task(move(task));
auto scheduled=move(chain).schedule_on(cap);
CHECK(scheduled.bound_capability()==root::capability_id(cap));
auto out=move(scheduled).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==9);
}
TEST_CASE(
"chain.then_on: success hop and transform",
"[chain.combinators]"){
OwnerCap cap{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{3}));
auto result=carrier::from_task(move(task)).then_on(cap,[](int x){return x*2;});
CHECK(result.bound_capability()==root::capability_id(cap));
auto out=move(result).release_outcome();
REQUIRE(out.is_success());
CHECK(out.success().value==6);
}
TEST_CASE(
"chain.then_on: cancel passes through without transform",
"[chain.combinators]"){
OwnerCap cap{};
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
bool called=false;
auto result=carrier::from_task(move(task)).then_on(cap,[&](int x){
called=true;
return x;
});
CHECK_FALSE(called);
CHECK(move(result).release_outcome().is_cancelled());
}
TEST_CASE(
"chain.into_task: success outcome becomes joinable Task",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{42}));
auto t=carrier::from_task(move(task)).into_task();
auto out=root::join(move(t));
REQUIRE(out.is_success());
CHECK(out.success().value==42);
}
TEST_CASE(
"chain.into_task: failure outcome becomes Task with failure",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_exception(make_exception_ptr(RE{"bad"})));
auto t=carrier::from_task(move(task)).into_task();
auto out=root::join(move(t));
CHECK(out.is_failure());
}
TEST_CASE(
"chain.into_task: cancelled outcome becomes Task with cancel",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
auto t=carrier::from_task(move(task)).into_task();
auto out=root::join(move(t));
CHECK(out.is_cancelled());
}
TEST_CASE(
"chain.into_task: eager chain",
"[chain.combinators]"){
auto[task,src]=root::make_task_source<int>();
REQUIRE(src.try_set_value(root::Success<int>{11}));
auto t=carrier::from_task(move(task)).into_task();
auto out=root::join(move(t));
REQUIRE(out.is_success());
CHECK(out.success().value==11);
}
