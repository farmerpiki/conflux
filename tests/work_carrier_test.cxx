// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier.model_a;

namespace root = conflux::work::root;
namespace model_a = conflux::work::carrier::model_a;

namespace {

struct OwnerCap : root::capability_id_from_address<OwnerCap> {};
struct DriverCap : root::capability_id_from_address<DriverCap> {};

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
