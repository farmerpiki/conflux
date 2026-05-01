// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier.model_a;

namespace root = conflux::work::root;
namespace model_a = conflux::work::carrier::model_a;

namespace {

struct OwnerCap : root::capability_id_from_address {};
struct DriverCap : root::capability_id_from_address {};

} // namespace

// ---------------------------------------------------------------------------
// Model A
// ---------------------------------------------------------------------------

TEST_CASE(
	"carrier.model_a: from_task preserves success outcome and task kind",
	"[carrier.model_a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{42}));

	auto chain = model_a::from_task(std::move(task));
	CHECK(chain.kind() == model_a::CarrierKind::task);

	auto out = std::move(chain).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 42);
}

TEST_CASE(
	"carrier.model_a: from_task preserves failure outcome",
	"[carrier.model_a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_failure(std::make_exception_ptr(std::runtime_error{"fail"})));

	auto chain = model_a::from_task(std::move(task));
	auto out = std::move(chain).release_outcome();
	REQUIRE(out.is_failure());
}

TEST_CASE(
	"carrier.model_a: from_task preserves cancelled outcome",
	"[carrier.model_a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_cancelled(root::CancelReason::requested));

	auto chain = model_a::from_task(std::move(task));
	auto out = std::move(chain).release_outcome();
	REQUIRE(out.is_cancelled());
	CHECK(out.cancelled().reason == root::CancelReason::requested);
}

TEST_CASE(
	"carrier.model_a: from_posted preserves outcome and posted kind",
	"[carrier.model_a]") {
	OwnerCap owner{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	REQUIRE(src.commit_success(root::Success<int>{7}));

	auto chain = model_a::from_posted(owner, std::move(posted));
	CHECK(chain.kind() == model_a::CarrierKind::posted);

	auto out = std::move(chain).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 7);
}

TEST_CASE(
	"carrier.model_a: from_operation preserves outcome and operation kind",
	"[carrier.model_a]") {
	DriverCap driver{};
	auto [op, src] = root::make_operation_source<int>(driver);
	REQUIRE(src.commit_success(root::Success<int>{5}));

	auto chain = model_a::from_operation(driver, std::move(op));
	CHECK(chain.kind() == model_a::CarrierKind::operation);

	auto out = std::move(chain).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 5);
}

TEST_CASE(
	"carrier.model_a: map transforms success value and preserves kind",
	"[carrier.model_a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{10}));

	auto chain = model_a::from_task(std::move(task));
	auto mapped = model_a::map(std::move(chain), [](int x) { return x * 3; });

	CHECK(mapped.kind() == model_a::CarrierKind::task);
	auto out = std::move(mapped).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 30);
}

TEST_CASE(
	"carrier.model_a: map passes through failure without calling fn",
	"[carrier.model_a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_failure(std::make_exception_ptr(std::runtime_error{"boom"})));

	bool fn_called = false;
	auto chain = model_a::from_task(std::move(task));
	auto mapped = model_a::map(std::move(chain), [&fn_called](int x) {
		fn_called = true;
		return x;
	});

	CHECK_FALSE(fn_called);
	auto out = std::move(mapped).release_outcome();
	CHECK(out.is_failure());
}

TEST_CASE(
	"carrier.model_a: map passes through cancelled without calling fn",
	"[carrier.model_a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_cancelled(root::CancelReason::shutdown));

	bool fn_called = false;
	auto chain = model_a::from_task(std::move(task));
	auto mapped = model_a::map(std::move(chain), [&fn_called](int x) {
		fn_called = true;
		return x;
	});

	CHECK_FALSE(fn_called);
	auto out = std::move(mapped).release_outcome();
	REQUIRE(out.is_cancelled());
	CHECK(out.cancelled().reason == root::CancelReason::shutdown);
}

TEST_CASE(
	"carrier.model_a: map wraps throwing fn as failure",
	"[carrier.model_a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{1}));

	auto chain = model_a::from_task(std::move(task));
	auto mapped = model_a::map(std::move(chain), [](int) -> int { throw std::runtime_error{"fn threw"}; });

	auto out = std::move(mapped).release_outcome();
	REQUIRE(out.is_failure());
	CHECK_THROWS_AS(std::rethrow_exception(out.failure().error), std::runtime_error);
}

TEST_CASE(
	"carrier.model_a: then delegates to map",
	"[carrier.model_a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{4}));

	auto chain = model_a::from_task(std::move(task));
	auto result = model_a::then(std::move(chain), [](int x) { return x + 6; });

	auto out = std::move(result).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 10);
}

TEST_CASE(
	"carrier.model_a: map chain 3 stages",
	"[carrier.model_a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{1}));

	auto c0 = model_a::from_task(std::move(task));
	auto c1 = model_a::map(std::move(c0), [](int x) { return x + 1; });
	auto c2 = model_a::map(std::move(c1), [](int x) { return x * 10; });
	auto c3 = model_a::map(std::move(c2), [](int x) { return x - 5; });

	auto out = std::move(c3).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 15);
}

TEST_CASE(
	"carrier.model_a: when_all both success produces Tup",
	"[carrier.model_a]") {
	auto [task_a, src_a] = root::make_task_source<int>();
	auto [task_b, src_b] = root::make_task_source<int>();
	REQUIRE(src_a.commit_success(root::Success<int>{3}));
	REQUIRE(src_b.commit_success(root::Success<int>{7}));

	auto ca = model_a::from_task(std::move(task_a));
	auto cb = model_a::from_task(std::move(task_b));
	auto combined = model_a::when_all(std::move(ca), std::move(cb));

	CHECK(combined.kind() == model_a::CarrierKind::task);
	auto out = std::move(combined).release_outcome();
	REQUIRE(out.is_success());
	CHECK(std::get<0>(out.success().value) == 3);
	CHECK(std::get<1>(out.success().value) == 7);
}

TEST_CASE(
	"carrier.model_a: when_all first failure takes priority",
	"[carrier.model_a]") {
	auto [task_a, src_a] = root::make_task_source<int>();
	auto [task_b, src_b] = root::make_task_source<int>();
	REQUIRE(src_a.commit_failure(std::make_exception_ptr(std::runtime_error{"a"})));
	REQUIRE(src_b.commit_success(root::Success<int>{1}));

	auto ca = model_a::from_task(std::move(task_a));
	auto cb = model_a::from_task(std::move(task_b));
	auto combined = model_a::when_all(std::move(ca), std::move(cb));

	auto out = std::move(combined).release_outcome();
	CHECK(out.is_failure());
}

TEST_CASE(
	"carrier.model_a: when_all second failure takes priority over success",
	"[carrier.model_a]") {
	auto [task_a, src_a] = root::make_task_source<int>();
	auto [task_b, src_b] = root::make_task_source<int>();
	REQUIRE(src_a.commit_success(root::Success<int>{1}));
	REQUIRE(src_b.commit_failure(std::make_exception_ptr(std::runtime_error{"b"})));

	auto ca = model_a::from_task(std::move(task_a));
	auto cb = model_a::from_task(std::move(task_b));
	auto combined = model_a::when_all(std::move(ca), std::move(cb));

	auto out = std::move(combined).release_outcome();
	CHECK(out.is_failure());
}

TEST_CASE(
	"carrier.model_a: when_all first cancel (no failure) yields cancelled",
	"[carrier.model_a]") {
	auto [task_a, src_a] = root::make_task_source<int>();
	auto [task_b, src_b] = root::make_task_source<int>();
	REQUIRE(src_a.commit_cancelled(root::CancelReason::requested));
	REQUIRE(src_b.commit_success(root::Success<int>{1}));

	auto ca = model_a::from_task(std::move(task_a));
	auto cb = model_a::from_task(std::move(task_b));
	auto combined = model_a::when_all(std::move(ca), std::move(cb));

	auto out = std::move(combined).release_outcome();
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"carrier.model_a: hop_to_posted changes kind to posted",
	"[carrier.model_a]") {
	OwnerCap owner{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{9}));

	auto chain = model_a::from_task(std::move(task));
	CHECK(chain.kind() == model_a::CarrierKind::task);

	auto hopped = model_a::hop_to_posted(owner, std::move(chain));
	CHECK(hopped.kind() == model_a::CarrierKind::posted);

	auto out = std::move(hopped).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 9);
}

TEST_CASE(
	"carrier.model_a: hop_to_operation changes kind to operation",
	"[carrier.model_a]") {
	DriverCap driver{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{11}));

	auto chain = model_a::from_task(std::move(task));
	auto hopped = model_a::hop_to_operation(driver, std::move(chain));
	CHECK(hopped.kind() == model_a::CarrierKind::operation);

	auto out = std::move(hopped).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 11);
}

TEST_CASE(
	"carrier.model_a: kind preserved through map",
	"[carrier.model_a]") {
	OwnerCap owner{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{2}));

	auto chain = model_a::from_task(std::move(task));
	auto hopped = model_a::hop_to_posted(owner, std::move(chain));
	auto mapped = model_a::map(std::move(hopped), [](int x) { return x * 2; });

	CHECK(mapped.kind() == model_a::CarrierKind::posted);
	auto out = std::move(mapped).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 4);
}

// ---------------------------------------------------------------------------
// E1.z — Chain member combinators
// ---------------------------------------------------------------------------

TEST_CASE(
	"chain.then: transforms success value",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{5}));
	auto out = std::move(model_a::from_task(std::move(task)).then([](int x) { return x * 3; })).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 15);
}

TEST_CASE(
	"chain.then: passes through failure without calling fn",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_failure(std::make_exception_ptr(std::runtime_error{"e"})));
	bool called = false;
	auto out = std::move(model_a::from_task(std::move(task)).then([&](int x) {
				   called = true;
				   return x;
			   })).release_outcome();
	CHECK_FALSE(called);
	CHECK(out.is_failure());
}

TEST_CASE(
	"chain.then: passes through cancel without calling fn",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_cancelled(root::CancelReason::requested));
	bool called = false;
	auto out = std::move(model_a::from_task(std::move(task)).then([&](int x) {
				   called = true;
				   return x;
			   })).release_outcome();
	CHECK_FALSE(called);
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"chain.then: fn throw becomes failure",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{1}));
	auto out = std::move(model_a::from_task(std::move(task)).then([](int) -> int {
				   throw std::runtime_error{"bad"};
			   })).release_outcome();
	CHECK(out.is_failure());
}

TEST_CASE(
	"chain.then: void T success applies fn",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<void>();
	REQUIRE(src.commit_success(root::Success<void>{}));
	int side = 0;
	auto out = std::move(model_a::from_task(std::move(task)).then([&] {
				   side = 42;
				   return side;
			   })).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 42);
	CHECK(side == 42);
}

TEST_CASE(
	"chain.catch_error: success passes through",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{7}));
	bool called = false;
	auto out = std::move(model_a::from_task(std::move(task)).catch_error([&](std::exception_ptr) {
				   called = true;
				   return -1;
			   })).release_outcome();
	CHECK_FALSE(called);
	REQUIRE(out.is_success());
	CHECK(out.success().value == 7);
}

TEST_CASE(
	"chain.catch_error: failure handled returning T",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_failure(std::make_exception_ptr(std::runtime_error{"e"})));
	auto out = std::move(model_a::from_task(std::move(task)).catch_error([](std::exception_ptr) {
				   return 99;
			   })).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 99);
}

TEST_CASE(
	"chain.catch_error: failure handled returning Chain<T>",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_failure(std::make_exception_ptr(std::runtime_error{"e"})));
	auto out = std::move(model_a::from_task(std::move(task)).catch_error([](std::exception_ptr) -> model_a::Chain<int> {
				   auto [t2, s2] = root::make_task_source<int>();
				   (void)s2.commit_success(root::Success<int>{55});
				   return model_a::from_task(std::move(t2));
			   })).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 55);
}

TEST_CASE(
	"chain.catch_error: cancel passes through",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_cancelled(root::CancelReason::requested));
	bool called = false;
	auto out = std::move(model_a::from_task(std::move(task)).catch_error([&](std::exception_ptr) {
				   called = true;
				   return -1;
			   })).release_outcome();
	CHECK_FALSE(called);
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"chain.on_cancel: cancel side-effect fires, still cancelled",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_cancelled(root::CancelReason::requested));
	bool called = false;
	auto out = std::move(model_a::from_task(std::move(task)).on_cancel([&] { called = true; })).release_outcome();
	CHECK(called);
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"chain.on_cancel: success passes through without calling fn",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{3}));
	bool called = false;
	auto out = std::move(model_a::from_task(std::move(task)).on_cancel([&] { called = true; })).release_outcome();
	CHECK_FALSE(called);
	REQUIRE(out.is_success());
	CHECK(out.success().value == 3);
}

TEST_CASE(
	"chain.on_cancel: throwing fn is swallowed; still cancelled",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_cancelled(root::CancelReason::requested));
	auto out = std::move(model_a::from_task(std::move(task)).on_cancel([] {
				   throw std::runtime_error{"x"};
			   })).release_outcome();
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"chain.recover_cancel: cancel recovery returns T",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_cancelled(root::CancelReason::requested));
	auto out = std::move(model_a::from_task(std::move(task)).recover_cancel([] { return 42; })).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 42);
}

TEST_CASE(
	"chain.recover_cancel: success passes through",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{8}));
	bool called = false;
	auto out = std::move(model_a::from_task(std::move(task)).recover_cancel([&] {
				   called = true;
				   return -1;
			   })).release_outcome();
	CHECK_FALSE(called);
	REQUIRE(out.is_success());
	CHECK(out.success().value == 8);
}

TEST_CASE(
	"chain.recover_cancel: failure passes through",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_failure(std::make_exception_ptr(std::runtime_error{"e"})));
	bool called = false;
	auto out = std::move(model_a::from_task(std::move(task)).recover_cancel([&] {
				   called = true;
				   return -1;
			   })).release_outcome();
	CHECK_FALSE(called);
	CHECK(out.is_failure());
}

TEST_CASE(
	"chain.recover: failure recovered",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_failure(std::make_exception_ptr(std::runtime_error{"e"})));
	auto out =
		std::move(model_a::from_task(std::move(task)).recover([](root::Outcome<int>) { return 77; })).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 77);
}

TEST_CASE(
	"chain.recover: cancel recovered",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_cancelled(root::CancelReason::requested));
	auto out =
		std::move(model_a::from_task(std::move(task)).recover([](root::Outcome<int>) { return 33; })).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 33);
}

TEST_CASE(
	"chain.recover: success passes through",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{6}));
	bool called = false;
	auto out = std::move(model_a::from_task(std::move(task)).recover([&](root::Outcome<int>) {
				   called = true;
				   return -1;
			   })).release_outcome();
	CHECK_FALSE(called);
	REQUIRE(out.is_success());
	CHECK(out.success().value == 6);
}

TEST_CASE(
	"chain.transform_outcome: success → success (identity)",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{4}));
	auto out = std::move(model_a::from_task(std::move(task)).transform_outcome([](root::Outcome<int> o) {
				   return o;
			   })).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 4);
}

TEST_CASE(
	"chain.transform_outcome: maps success value to different type",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{10}));
	auto out = std::move(
				   model_a::from_task(std::move(task))
					   .transform_outcome([](root::Outcome<int> o) -> root::Outcome<std::string> {
						   if (o.is_success()) {
							   return root::Outcome<std::string>{
								   root::Success<std::string>{std::to_string(o.success().value)}};
						   }
						   return root::Outcome<std::string>{std::move(o).failure()};
					   }))
				   .release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == "10");
}

TEST_CASE(
	"chain.schedule_on: binds capability, preserves outcome",
	"[chain.combinators]") {
	OwnerCap cap{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{9}));
	auto chain = model_a::from_task(std::move(task));
	auto scheduled = std::move(chain).schedule_on(cap);
	CHECK(scheduled.bound_capability() == root::capability_id(cap));
	auto out = std::move(scheduled).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 9);
}

TEST_CASE(
	"chain.then_on: success hop and transform",
	"[chain.combinators]") {
	OwnerCap cap{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{3}));
	auto result = model_a::from_task(std::move(task)).then_on(cap, [](int x) { return x * 2; });
	CHECK(result.bound_capability() == root::capability_id(cap));
	auto out = std::move(result).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 6);
}

TEST_CASE(
	"chain.then_on: cancel passes through without transform",
	"[chain.combinators]") {
	OwnerCap cap{};
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_cancelled(root::CancelReason::requested));
	bool called = false;
	auto result = model_a::from_task(std::move(task)).then_on(cap, [&](int x) {
		called = true;
		return x;
	});
	CHECK_FALSE(called);
	CHECK(std::move(result).release_outcome().is_cancelled());
}

TEST_CASE(
	"chain.into_task: success outcome becomes joinable Task",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{42}));
	auto t = model_a::from_task(std::move(task)).into_task();
	auto out = root::join(std::move(t));
	REQUIRE(out.is_success());
	CHECK(out.success().value == 42);
}

TEST_CASE(
	"chain.into_task: failure outcome becomes Task with failure",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_failure(std::make_exception_ptr(std::runtime_error{"bad"})));
	auto t = model_a::from_task(std::move(task)).into_task();
	auto out = root::join(std::move(t));
	CHECK(out.is_failure());
}

TEST_CASE(
	"chain.into_task: cancelled outcome becomes Task with cancel",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_cancelled(root::CancelReason::requested));
	auto t = model_a::from_task(std::move(task)).into_task();
	auto out = root::join(std::move(t));
	CHECK(out.is_cancelled());
}

TEST_CASE(
	"chain.into_task_unchecked: same as into_task for eager chain",
	"[chain.combinators]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{11}));
	auto t = model_a::from_task(std::move(task)).into_task_unchecked();
	auto out = root::join(std::move(t));
	REQUIRE(out.is_success());
	CHECK(out.success().value == 11);
}
