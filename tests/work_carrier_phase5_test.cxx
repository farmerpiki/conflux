// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier;
import conflux.work.carrier.coro;
namespace {

// Minimal coroutine harness: runs to completion synchronously if await_ready().
// For the async path, caller must commit the source before calling get().
template<class T>
struct SyncTask {
	struct promise_type {
		Opt<T> value_;
		Opt<EP> error_;
		SyncTask get_return_object() { return SyncTask{std::coroutine_handle<promise_type>::from_promise(*this)}; }
		std::suspend_never initial_suspend() noexcept { return {}; }
		std::suspend_always final_suspend() noexcept { return {}; }
		void return_value(
			T v) {
			value_ = move(v);
		}
		void unhandled_exception() { error_ = current_exception(); }
	};
	std::coroutine_handle<promise_type> h_;
	explicit SyncTask(
		std::coroutine_handle<promise_type> h)
		: h_{h} {}
	~SyncTask() {
		if (h_) {
			h_.destroy();
		}
	}
	SyncTask(
		SyncTask &&o) noexcept
		: h_{exchange(o.h_, {})} {}
	T get() {
		if (!h_.done()) {
			h_.resume();
		}
		auto &p = h_.promise();
		if (p.error_) {
			rethrow_exception(*p.error_);
		}
		return move(*p.value_);
	}
};
template<>
struct SyncTask<void> {
	struct promise_type {
		Opt<EP> error_;
		SyncTask get_return_object() { return SyncTask{std::coroutine_handle<promise_type>::from_promise(*this)}; }
		std::suspend_never initial_suspend() noexcept { return {}; }
		std::suspend_always final_suspend() noexcept { return {}; }
		void return_void() {}
		void unhandled_exception() { error_ = current_exception(); }
	};
	std::coroutine_handle<promise_type> h_;
	explicit SyncTask(
		std::coroutine_handle<promise_type> h)
		: h_{h} {}
	~SyncTask() {
		if (h_) {
			h_.destroy();
		}
	}
	SyncTask(
		SyncTask &&o) noexcept
		: h_{exchange(o.h_, {})} {}
	void get() {
		if (!h_.done()) {
			h_.resume();
		}
		auto &p = h_.promise();
		if (p.error_) {
			rethrow_exception(*p.error_);
		}
	}
};

} // anonymous namespace

namespace root = conflux::work::root;
namespace carrier = conflux::work::carrier;
// ---------------------------------------------------------------------------
// Phase 5a: Chain<T> co_await
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase5a: co_await Chain<int> returns value on success",
	"[phase5a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{7}));
	auto chain = carrier::from_task(move(task));
	auto awaiter = move(chain).operator co_await();
	CHECK(awaiter.await_ready());
	int const v = awaiter.await_resume();
	CHECK(v == 7);
}
TEST_CASE(
	"phase5a: co_await Chain<int> rethrows on failure",
	"[phase5a]") {
	auto [task, src] = root::make_task_source<int>();
	auto ex = make_exception_ptr(RE{"fail"});
	REQUIRE(src.try_set_exception(ex));
	auto chain = carrier::from_task(move(task));

	auto awaiter = move(chain).operator co_await();
	CHECK_THROWS_AS(awaiter.await_resume(), RE);
}
TEST_CASE(
	"phase5a: co_await Chain<int> throws CancelledError on cancellation",
	"[phase5a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
	auto chain = carrier::from_task(move(task));

	auto awaiter = move(chain).operator co_await();
	CHECK_THROWS_AS(awaiter.await_resume(), root::CancelledError);
}
TEST_CASE(
	"phase5a: ChainAwaiter::await_ready always returns true",
	"[phase5a]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{1}));
	auto chain = carrier::from_task(move(task));
	auto awaiter = move(chain).operator co_await();
	CHECK(awaiter.await_ready());
}
// ---------------------------------------------------------------------------
// Phase 5b: EagerChain<T>
// ---------------------------------------------------------------------------

carrier::EagerChain<int> eager_double(
	carrier::Chain<int> input) {
	int const v = co_await move(input);
	co_return v * 2;
}
TEST_CASE(
	"phase5b: EagerChain processes success via co_await Chain<int>",
	"[phase5b]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{6}));
	auto chain = carrier::from_task(move(task));

	auto out = eager_double(move(chain)).chain().release_outcome();
	REQUIRE(out.is_success());
}
TEST_CASE(
	"phase5b: EagerChain propagates failure through co_await Chain",
	"[phase5b]") {
	auto [task, src] = root::make_task_source<int>();
	auto ex = make_exception_ptr(RE{"boom"});
	REQUIRE(src.try_set_exception(ex));
	auto chain = carrier::from_task(move(task));

	auto out = eager_double(move(chain)).chain().release_outcome();
	REQUIRE(out.is_failure());
}
TEST_CASE(
	"phase5b: EagerChain propagates cancellation as failure through co_await Chain",
	"[phase5b]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
	auto chain = carrier::from_task(move(task));

	// co_await throws CancelledError; unhandled_exception stores it as Failure.
	auto out = eager_double(move(chain)).chain().release_outcome();
	REQUIRE(out.is_failure());
	CHECK_THROWS_AS(rethrow_exception(out.failure().error), root::CancelledError);
}
carrier::EagerChain<int> eager_nested(
	carrier::Chain<int> input) {
	int const v = co_await eager_double(move(input));
	co_return v + 1;
}
TEST_CASE(
	"phase5b: EagerChain nested co_await of EagerChain works",
	"[phase5b]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{3}));
	auto chain = carrier::from_task(move(task));

	auto out = eager_nested(move(chain)).chain().release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 7);
}
carrier::EagerChain<int> eager_produces_value() {
	co_return 42;
}
TEST_CASE(
	"phase5b: EagerChain returning value directly co_returns correctly",
	"[phase5b]") {
	auto out = eager_produces_value().chain().release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 42);
}
carrier::EagerChain<void> eager_void_fn(
	carrier::Chain<int> input) {
	(void)(co_await move(input));
}
TEST_CASE(
	"phase5b: EagerChain<void> co_returns void on success",
	"[phase5b]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{0}));
	auto chain = carrier::from_task(move(task));

	auto out = eager_void_fn(move(chain)).chain().release_outcome();
	CHECK(out.is_success());
}
TEST_CASE(
	"phase5b: EagerChain feeds into model_a map combinator",
	"[phase5b]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{5}));
	auto chain = carrier::from_task(move(task));

	auto mapped = carrier::map(eager_double(move(chain)).chain(), [](int v) { return v + 100; });
	auto out = move(mapped).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 110);
}
// ---------------------------------------------------------------------------
// Phase 5c: TaskHandleAwaiter / TaskHandleChainAwaiter
// ---------------------------------------------------------------------------
// Note: operator co_await(TaskJoinHandle&&) lives in model_a namespace, not
// root, so ADL from a plain coroutine body won't find it. Tests construct
// TaskHandleAwaiter / TaskHandleChainAwaiter explicitly.

// --- already-ready sync path ---

SyncTask<int> coro_await_task_handle_success(
	root::TaskJoinHandle<int> jh) {
	co_return co_await carrier::TaskHandleAwaiter<int>{move(jh)};
}
TEST_CASE(
	"phase5c: co_await TaskJoinHandle<int> sync path returns value on success",
	"[phase5c]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	REQUIRE(src.try_set_value(root::Success<int>{99}));
	CHECK(coro_await_task_handle_success(move(jh)).get() == 99);
}
SyncTask<int> coro_await_task_handle_failure(
	root::TaskJoinHandle<int> jh) {
	co_return co_await carrier::TaskHandleAwaiter<int>{move(jh)};
}
TEST_CASE(
	"phase5c: co_await TaskJoinHandle<int> sync path rethrows on failure",
	"[phase5c]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	auto ex = make_exception_ptr(RE{"task failed"});
	REQUIRE(src.try_set_exception(ex));
	CHECK_THROWS_AS(coro_await_task_handle_failure(move(jh)).get(), RE);
}
SyncTask<void> coro_await_task_handle_cancel(
	root::TaskJoinHandle<int> jh) {
	(void)(co_await carrier::TaskHandleAwaiter<int>{move(jh)});
}
TEST_CASE(
	"phase5c: co_await TaskJoinHandle<int> sync path throws CancelledError on cancel",
	"[phase5c]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
	CHECK_THROWS_AS(coro_await_task_handle_cancel(move(jh)).get(), root::CancelledError);
}
// --- TaskHandleAwaiter::await_ready ---

TEST_CASE(
	"phase5c: TaskHandleAwaiter::await_ready true when result committed before co_await",
	"[phase5c]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	REQUIRE(src.try_set_value(root::Success<int>{1}));
	carrier::TaskHandleAwaiter<int> awaiter{move(jh)};
	CHECK(awaiter.await_ready());
}
TEST_CASE(
	"phase5c: TaskHandleAwaiter::await_ready false when result not yet committed",
	"[phase5c]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	carrier::TaskHandleAwaiter<int> awaiter{move(jh)};
	CHECK_FALSE(awaiter.await_ready());
	REQUIRE(src.try_set_value(root::Success<int>{0}));
}
// --- async suspension path (commit from another thread) ---

SyncTask<int> coro_async_await(
	root::TaskJoinHandle<int> jh) {
	co_return co_await carrier::TaskHandleAwaiter<int>{move(jh)};
}
TEST_CASE(
	"phase5c: co_await TaskJoinHandle<int> async path — commit from thread resumes coroutine",
	"[phase5c]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	auto coro = coro_async_await(move(jh));
	// coroutine started eagerly, suspended at co_await (not yet ready)
	thread t{[s = move(src)]() mutable { (void)s.try_set_value(root::Success<int>{77}); }};
	t.join();
	CHECK(coro.get() == 77);
}
// --- await_chain ---

SyncTask<carrier::Chain<int>> coro_await_chain_success(
	root::TaskJoinHandle<int> jh) {
	co_return co_await carrier::TaskHandleChainAwaiter<int>{move(jh)};
}
TEST_CASE(
	"phase5c: await_chain returns Chain<int> with success outcome",
	"[phase5c]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	REQUIRE(src.try_set_value(root::Success<int>{55}));
	auto chain = coro_await_chain_success(move(jh)).get();
	auto out = move(chain).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 55);
}
SyncTask<carrier::Chain<int>> coro_await_chain_failure(
	root::TaskJoinHandle<int> jh) {
	co_return co_await carrier::TaskHandleChainAwaiter<int>{move(jh)};
}
TEST_CASE(
	"phase5c: await_chain returns Chain<int> with failure outcome (no throw)",
	"[phase5c]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	auto ex = make_exception_ptr(RE{"chain fail"});
	REQUIRE(src.try_set_exception(ex));
	auto chain = coro_await_chain_failure(move(jh)).get();
	auto out = move(chain).release_outcome();
	CHECK(out.is_failure());
}
SyncTask<carrier::Chain<int>> coro_await_chain_cancel(
	root::TaskJoinHandle<int> jh) {
	co_return co_await carrier::TaskHandleChainAwaiter<int>{move(jh)};
}
TEST_CASE(
	"phase5c: await_chain returns Chain<int> with cancelled outcome (no throw)",
	"[phase5c]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	REQUIRE(src.try_set_cancelled(root::work_errc::cancelled_requested));
	auto chain = coro_await_chain_cancel(move(jh)).get();
	auto out = move(chain).release_outcome();
	CHECK(out.is_cancelled());
}
// --- unconsumed awaiter defensive abandon (must not terminate) ---

TEST_CASE(
	"phase5c: TaskHandleAwaiter destroyed unconsumed without terminating",
	"[phase5c]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(move(task));
	REQUIRE(src.try_set_value(root::Success<int>{0}));
	{
		carrier::TaskHandleAwaiter<int> awaiter{move(jh)};
		(void)awaiter.await_ready();
		// destroy without calling await_resume — defensive abandon fires in dtor
	}
	CHECK(true);
}
