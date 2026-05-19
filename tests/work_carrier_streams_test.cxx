// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier;
import conflux.work.carrier.streams;

namespace root = conflux::work::root;
namespace carrier = conflux::work::carrier;
namespace carrier = conflux::work::carrier;
namespace {

// Minimal coroutine harness for async suspension tests.
template<class T>
struct SyncTask {
	struct promise_type {
		std::optional<T> value_;
		std::optional<std::exception_ptr> error_;
		SyncTask get_return_object() { return SyncTask{std::coroutine_handle<promise_type>::from_promise(*this)}; }
		std::suspend_never initial_suspend() noexcept { return {}; }
		std::suspend_always final_suspend() noexcept { return {}; }
		void return_value(
			T v) {
			value_ = std::move(v);
		}
		void unhandled_exception() { error_ = std::current_exception(); }
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
		: h_{std::exchange(o.h_, {})} {}
	T get() {
		if (!h_.done()) {
			h_.resume();
		}
		auto &p = h_.promise();
		if (p.error_) {
			rethrow_exception(*p.error_);
		}
		return std::move(*p.value_);
	}
};
SyncTask<carrier::Chain<int>> coro_droppable_slot(
	carrier::DroppableSlot<int> slot) {
	co_return co_await std::move(slot);
}

} // namespace
// ---------------------------------------------------------------------------
// Phase 8a: DroppableSlot — basic drop behaviour
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase8a: DroppableSlot dtor does not terminate when dropped unconsumed (unready)",
	"[phase8a]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(std::move(task));
	{
		carrier::DroppableSlot<int> const slot{std::move(jh)};
	} // drain installed; handle will be consumed when src commits
	(void)src.try_set_value(root::Success<int>{1});
	// Reaching here without std::terminate means the test passes.
}
TEST_CASE(
	"phase8a: DroppableSlot dtor does not terminate when dropped after result ready",
	"[phase8a]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{2});
	auto jh = root::into_join_handle(std::move(task));
	{ carrier::DroppableSlot<int> const slot{std::move(jh)}; } // ready path: join inline in dtor
}
TEST_CASE(
	"phase8a: ready() reflects committed state",
	"[phase8a]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(std::move(task));
	carrier::DroppableSlot<int> slot{std::move(jh)};
	CHECK_FALSE(slot.ready());
	(void)src.try_set_value(root::Success<int>{3});
	CHECK(slot.ready());
	(void)std::move(slot).wait();
}
// ---------------------------------------------------------------------------
// Phase 8b: on_drop fires in expected conditions
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase8b: on_drop fires with ready outcome when slot dropped unconsumed (ready path)",
	"[phase8b]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{42});
	auto jh = root::into_join_handle(std::move(task));

	int drop_value = -1;
	{
		carrier::DroppableSlot<int> slot{std::move(jh)};
		slot.on_drop([&](root::Outcome<int> out) noexcept {
			if (out.is_success()) {
				drop_value = out.success().value;
			}
		});
	}
	CHECK(drop_value == 42);
}
TEST_CASE(
	"phase8b: on_drop fires with late outcome when slot dropped before ready (async drain)",
	"[phase8b]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(std::move(task));

	int drop_value = -1;
	{
		carrier::DroppableSlot<int> slot{std::move(jh)};
		slot.on_drop([&](root::Outcome<int> out) noexcept {
			if (out.is_success()) {
				drop_value = out.success().value;
			}
		});
	} // drain hook installed here

	// Drain fires synchronously on commit std::thread during try_set_value.
	(void)src.try_set_value(root::Success<int>{99});
	CHECK(drop_value == 99);
}
TEST_CASE(
	"phase8b: on_drop fires with failure outcome",
	"[phase8b]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(std::move(task));

	bool drop_saw_failure = false;
	{
		carrier::DroppableSlot<int> slot{std::move(jh)};
		slot.on_drop([&](root::Outcome<int> const &out) noexcept { drop_saw_failure = out.is_failure(); });
	}
	(void)src.try_set_exception(make_exception_ptr(std::runtime_error{"fail"}));
	CHECK(drop_saw_failure);
}
TEST_CASE(
	"phase8b: on_drop fires with cancelled outcome",
	"[phase8b]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(std::move(task));

	bool drop_saw_cancel = false;
	{
		carrier::DroppableSlot<int> slot{std::move(jh)};
		slot.on_drop([&](root::Outcome<int> const &out) noexcept { drop_saw_cancel = out.is_cancelled(); });
	}
	(void)src.try_set_cancelled(root::work_errc::cancelled_requested);
	CHECK(drop_saw_cancel);
}
// ---------------------------------------------------------------------------
// Phase 8c: on_drop does NOT fire when result was consumed
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase8c: on_drop does not fire when consumed via wait()",
	"[phase8c]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{7});
	auto jh = root::into_join_handle(std::move(task));

	bool fired = false;
	carrier::DroppableSlot<int> slot{std::move(jh)};
	slot.on_drop([&](root::Outcome<int> const &) noexcept { fired = true; });
	auto chain = std::move(slot).wait();
	auto out = std::move(chain).release_outcome();
	CHECK(out.success().value == 7);
	CHECK_FALSE(fired);
}
TEST_CASE(
	"phase8c: on_drop does not fire when consumed via try_get()",
	"[phase8c]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{8});
	auto jh = root::into_join_handle(std::move(task));

	bool fired = false;
	carrier::DroppableSlot<int> slot{std::move(jh)};
	slot.on_drop([&](root::Outcome<int> const &) noexcept { fired = true; });
	auto result = std::move(slot).try_get();
	REQUIRE(result.has_value());
	CHECK(result->success().value == 8);
	CHECK_FALSE(fired);
}
// ---------------------------------------------------------------------------
// Phase 8d: try_get non-blocking behaviour
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase8d: try_get returns std::nullopt when not yet ready",
	"[phase8d]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(std::move(task));
	carrier::DroppableSlot<int> slot{std::move(jh)};
	auto result = std::move(slot).try_get();
	CHECK_FALSE(result.has_value());
	// Slot dtor drains; commit satisfies liveness.
	(void)src.try_set_cancelled(root::work_errc::cancelled_requested);
}
TEST_CASE(
	"phase8d: try_get returns outcome when ready",
	"[phase8d]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{11});
	auto jh = root::into_join_handle(std::move(task));
	carrier::DroppableSlot<int> slot{std::move(jh)};
	auto result = std::move(slot).try_get();
	REQUIRE(result.has_value());
	CHECK(result->success().value == 11);
}
// ---------------------------------------------------------------------------
// Phase 8e: producer abandon without commit satisfies liveness
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase8e: drop unready slot then abandon producer satisfies liveness",
	"[phase8e]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(std::move(task));
	{ carrier::DroppableSlot<int> const slot{std::move(jh)}; } // drain hook installed
	// ~BasicSource fires try_set_cancelled(abandoned) → drain hook fires → liveness satisfied
	// If liveness were violated, ~TaskJoinHandle would std::terminate.
}
// ---------------------------------------------------------------------------
// Phase 8f: co_await DroppableSlot
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase8f: co_await DroppableSlot sync path returns Chain<int> on success",
	"[phase8f]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{55});
	auto jh = root::into_join_handle(std::move(task));
	auto coro = coro_droppable_slot(carrier::DroppableSlot<int>{std::move(jh)});
	auto chain = coro.get();
	auto out = std::move(chain).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 55);
}
TEST_CASE(
	"phase8f: co_await DroppableSlot async path — commit from std::thread resumes coroutine",
	"[phase8f]") {
	auto [task, src] = root::make_task_source<int>();
	auto jh = root::into_join_handle(std::move(task));
	auto coro = coro_droppable_slot(carrier::DroppableSlot<int>{std::move(jh)});
	// coroutine started eagerly, suspended at co_await (not yet ready)
	std::thread t{[s = std::move(src)]() mutable { (void)s.try_set_value(root::Success<int>{77}); }};
	t.join();
	auto chain = coro.get();
	auto out = std::move(chain).release_outcome();
	REQUIRE(out.is_success());
	CHECK(out.success().value == 77);
}
TEST_CASE(
	"phase8f: on_drop does not fire when consumed via co_await",
	"[phase8f]") {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{33});
	auto jh = root::into_join_handle(std::move(task));

	bool fired = false;
	carrier::DroppableSlot<int> slot{std::move(jh)};
	slot.on_drop([&](root::Outcome<int> const &) noexcept { fired = true; });
	auto coro = coro_droppable_slot(std::move(slot));
	auto chain = coro.get();
	(void)chain;
	CHECK_FALSE(fired);
}
// ---------------------------------------------------------------------------
// Phase 8g: CoalescingSlot
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase8g: CoalescingSlot take returns committed value",
	"[phase8g]") {
	carrier::CoalescingSlot<int> slot{};
	slot.commit(42);
	auto result = slot.take();
	REQUIRE(result.has_value());
	CHECK(*result == 42);
}
TEST_CASE(
	"phase8g: CoalescingSlot take returns std::nullopt when empty",
	"[phase8g]") {
	carrier::CoalescingSlot<int> slot{};
	CHECK_FALSE(slot.take().has_value());
}
TEST_CASE(
	"phase8g: CoalescingSlot latest commit wins",
	"[phase8g]") {
	carrier::CoalescingSlot<int> slot{};
	slot.commit(1);
	slot.commit(2);
	slot.commit(3);
	auto result = slot.take();
	REQUIRE(result.has_value());
	CHECK(*result == 3);
}
TEST_CASE(
	"phase8g: CoalescingSlot available reflects slot state",
	"[phase8g]") {
	carrier::CoalescingSlot<int> slot{};
	CHECK_FALSE(slot.available());
	slot.commit(5);
	CHECK(slot.available());
	(void)slot.take();
	CHECK_FALSE(slot.available());
}
TEST_CASE(
	"phase8g: CoalescingSlot concurrent commit + take",
	"[phase8g]") {
	carrier::CoalescingSlot<int> slot{};
	std::atomic<int> last_seen{-1};
	constexpr int kIterations = 10000;

	std::thread producer{[&] {
		for (int i = 0; i < kIterations; ++i) {
			slot.commit(i);
		}
	}};

	std::thread consumer{[&] {
		for (int i = 0; i < kIterations; ++i) {
			if (auto v = slot.take()) {
				last_seen = *v;
			}
		}
	}};

	producer.join();
	consumer.join();
	// Drain any remaining value — slot must be internally consistent.
	(void)slot.take();
}
