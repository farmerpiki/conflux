// Semantic compatibility test for the E1.1 deprecated API surface.
// Verifies deprecated symbols still work correctly at runtime.
// Warnings are suppressed project-wide via CONFLUX_SUPPRESS_DEPRECATION_WARNINGS=ON.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;

namespace root = conflux::work::root;

// ---------------------------------------------------------------------------
// sync_wait (deprecated: use root::join() + Outcome<T>)
// ---------------------------------------------------------------------------

TEST_CASE(
	"compat: sync_wait returns value",
	"[work][compat]") {
	WorkPool pool;
	auto result = sync_wait(run_on_task(pool, [] { return 99; }));
	CHECK(result == 99);
}

TEST_CASE(
	"compat: sync_wait rethrows failure",
	"[work][compat]") {
	WorkPool pool;
	bool caught = false;
	try {
		sync_wait(run_on_task(pool, []() -> int { throw RE{"compat-fail"}; }));
	} catch (RE const &e) { caught = SV{e.what()} == "compat-fail"; }
	CHECK(caught);
}

TEST_CASE(
	"compat: sync_wait throws Cancelled on pool stop",
	"[work][compat]") {
	WorkPool pool;
	pool.stop();
	bool cancelled = false;
	try {
		sync_wait(run_on_task(pool, [] { return 0; }));
	} catch (::Cancelled const &) { cancelled = true; }
	CHECK(cancelled);
}

// ---------------------------------------------------------------------------
// co_spawn (deprecated: use Task<T>::detach())
// ---------------------------------------------------------------------------

TEST_CASE(
	"compat: co_spawn fires old-style coroutine",
	"[work][compat]") {
	WorkPool pool;
	Atom<int> counter{0};
	auto gate = std::make_shared<std::barrier<>>(2);
	co_spawn([](root::Task<void> t) -> ::Task<void> { co_await std::move(t); }(run_on_task(pool, [gate, &counter] {
				 counter.fetch_add(1, std::memory_order_release);
				 gate->arrive_and_wait();
			 })));
	gate->arrive_and_wait();
	CHECK(counter.load(std::memory_order_acquire) == 1);
}

// ---------------------------------------------------------------------------
// FlowSource<T> (deprecated: use root::TaskSource<T>)
// Tested via the old ::Task<T> coroutine mechanism.
// ---------------------------------------------------------------------------

namespace {

auto coro_with_flow_source(
	int val) -> ::Task<void> {
	FlowSource<int> src;
	src.resolve(val);
	[[maybe_unused]] auto v = co_await src.flow();
	// confirm the value is correct via Catch2 — this runs in the coroutine
	REQUIRE(v == val);
}

auto coro_flow_source_reject() -> ::Task<void> {
	FlowSource<int> src;
	src.reject(std::make_exception_ptr(RE{"flow-err"}));
	try {
		[[maybe_unused]] auto v = co_await src.flow();
		FAIL("expected exception");
	} catch (RE const &e) { CHECK(SV{e.what()} == "flow-err"); }
}

auto coro_flow_source_cancel() -> ::Task<void> {
	FlowSource<int> src;
	src.cancel();
	try {
		[[maybe_unused]] auto v = co_await src.flow();
		FAIL("expected Cancelled");
	} catch (::Cancelled const &) {
		// expected
		SUCCEED();
	}
}

auto coro_flow_source_idempotent(
	int val) -> ::Task<void> {
	FlowSource<int> src;
	src.resolve(val);
	src.resolve(val + 99); // second resolve must be no-op
	src.reject(std::make_exception_ptr(RE{"should not see"}));
	auto v = co_await src.flow();
	REQUIRE(v == val);
}

} // namespace

TEST_CASE(
	"compat: FlowSource resolve propagates value through Task coroutine",
	"[work][compat]") {
	std::latch done{1};
	co_spawn([&done]() -> ::Task<void> {
		co_await coro_with_flow_source(42);
		done.count_down();
	}());
	done.wait();
}

TEST_CASE(
	"compat: FlowSource reject propagates error through Task coroutine",
	"[work][compat]") {
	std::latch done{1};
	co_spawn([&done]() -> ::Task<void> {
		co_await coro_flow_source_reject();
		done.count_down();
	}());
	done.wait();
}

TEST_CASE(
	"compat: FlowSource cancel propagates through Task coroutine",
	"[work][compat]") {
	std::latch done{1};
	co_spawn([&done]() -> ::Task<void> {
		co_await coro_flow_source_cancel();
		done.count_down();
	}());
	done.wait();
}

TEST_CASE(
	"compat: FlowSource only first resolve takes effect",
	"[work][compat]") {
	std::latch done{1};
	co_spawn([&done]() -> ::Task<void> {
		co_await coro_flow_source_idempotent(7);
		done.count_down();
	}());
	done.wait();
}
