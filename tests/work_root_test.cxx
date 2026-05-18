// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work.root;

namespace root = conflux::work::root;
namespace {

struct OwnerCap {};
struct DriverCap {};
struct InnerCap {};
struct OuterCap : InnerCap {};
root::Task<int> await_task_value(
	root::Task<int> task) {
	co_return co_await move(task);
}

#if CONFLUX_WORK_ALLOC_STATS
root::Task<int> stats_probe_task() {
	co_return 7;
}
#endif

} // namespace
namespace conflux::work::root {

template<>
inline constexpr bool enable_address_capability_v<OwnerCap> = true;
template<>
inline constexpr bool enable_address_capability_v<DriverCap> = true;
template<>
inline constexpr bool enable_address_capability_v<InnerCap> = true;
template<>
inline constexpr bool enable_address_capability_v<OuterCap> = true;

} // namespace conflux::work::root
namespace {

struct ThrowOnCopy {
	inline static bool throw_now = false;
	int v = 0;

	ThrowOnCopy() = default;
	explicit ThrowOnCopy(
		int x)
		: v{x} {}
	ThrowOnCopy(
		ThrowOnCopy const &other)
		: v{other.v} {
		if (throw_now) {
			throw std::runtime_error{"copy boom"};
		}
	}
	ThrowOnCopy &operator =(ThrowOnCopy const &) = default;
	ThrowOnCopy(ThrowOnCopy &&) noexcept = default;
	ThrowOnCopy &operator =(ThrowOnCopy &&) noexcept = default;
};

} // namespace

#if CONFLUX_WORK_ALLOC_STATS
TEST_CASE(
	"work.root: optional allocation stats count control blocks and coroutine frames",
	"[work.root]") {
	root::reset_task_allocation_stats();
	{
		auto [task, src] = root::make_task_source<int>();
		auto stats = root::task_allocation_stats();
		CHECK(stats.control_block_allocations == 1);
		CHECK(stats.control_block_deallocations == 0);
		REQUIRE(src.try_set_value(root::Success<int>{3}));
		CHECK(root::value(move(task)) == 3);
	}
	{
		auto stats = root::task_allocation_stats();
		CHECK(stats.control_block_allocations == 1);
		CHECK(stats.control_block_deallocations == 1);
	}

	root::reset_task_allocation_stats();
	{
		auto task = stats_probe_task();
		CHECK(root::value(move(task)) == 7);
	}
	auto stats = root::task_allocation_stats();
	CHECK(stats.control_block_allocations >= 1);
	CHECK(stats.control_block_deallocations == stats.control_block_allocations);
	CHECK(stats.coroutine_frame_deallocations == stats.coroutine_frame_allocations);
}
#endif

TEST_CASE(
	"work.root: Outcome copy assignment for copyable payload succeeds",
	"[work.root]") {
	root::Outcome<int> src{root::Success<int>{41}};
	root::Outcome<int> dst{root::Success<int>{1}};
	dst = src;

	REQUIRE(dst.is_success());
	CHECK(dst.success().value == 41);
	REQUIRE(src.is_success());
	CHECK(src.success().value == 41);
}
TEST_CASE(
	"work.root: Outcome copy assignment keeps destination unchanged on throw",
	"[work.root]") {
	root::Outcome<ThrowOnCopy> src{root::Success<ThrowOnCopy>{ThrowOnCopy{9}}};
	root::Outcome<ThrowOnCopy> dst{root::Success<ThrowOnCopy>{ThrowOnCopy{7}}};

	ThrowOnCopy::throw_now = true;
	CHECK_THROWS_AS(dst = src, std::runtime_error);
	ThrowOnCopy::throw_now = false;

	REQUIRE(dst.is_success());
	CHECK(dst.success().value.v == 7);
	REQUIRE(src.is_success());
	CHECK(src.success().value.v == 9);
}
TEST_CASE(
	"work.root: Failure and FailureError normalize null EP",
	"[work.root]") {
	root::Failure f{std::exception_ptr{}};
	REQUIRE(f.error != nullptr);

	root::FailureError const err{std::exception_ptr{}};
	REQUIRE(err.cause() != nullptr);
	CHECK_THROWS(err.rethrow_cause());
}
TEST_CASE(
	"work.root: task join returns committed success",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{42}));
	auto val = root::value(move(task));
	CHECK(val == 42);
}
TEST_CASE(
	"work.root: try_join_ready leaves pending task joinable",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	auto pending = root::try_join_ready(move(task));
	CHECK_FALSE(pending.has_value());
	REQUIRE(src.try_set_value(root::Success<int>{5}));
	auto ready = root::try_join_ready(move(task));
	REQUIRE(ready.has_value());
	REQUIRE(ready->is_success());
	CHECK(ready->success().value == 5);
}
TEST_CASE(
	"work.root: join_ready rejects pending task without consuming it",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	CHECK_THROWS_AS(root::join_ready(move(task)), root::JoinError);
	REQUIRE(src.try_set_value(root::Success<int>{6}));
	auto out = root::blocking_join(move(task));
	REQUIRE(out.is_success());
	CHECK(out.success().value == 6);
}
TEST_CASE(
	"work.root: source destructor fallback commits abandoned cancel",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	{
		auto src_holder = std::make_optional(move(src));
		src_holder.reset();
	}
	auto out = root::blocking_join(move(task));
	REQUIRE(out.is_cancelled());
	CHECK(out.cancelled().reason == root::CancelReason::abandoned);
}
TEST_CASE(
	"work.root: guard_abandon release disarms destructor abandon",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	{
		auto guard = root::guard_abandon(move(task));
		task = move(guard).release();
	}
	REQUIRE(src.try_set_value(root::Success<int>{17}));
	auto val17 = root::value(move(task));
	CHECK(val17 == 17);
}
TEST_CASE(
	"work.root: request_cancel first call wins",
	"[work.root]") {
	auto [control, src] = root::make_task_control_source<int>();
	CHECK(control.request_cancel());
	CHECK_FALSE(control.request_cancel());
	CHECK(control.cancel_requested());
	(void)src;
}
TEST_CASE(
	"work.root: no-cancellation admission yields inert stop_token",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>(root::SubmitOptions{.enable_cancellation = false});
	CHECK_FALSE(src.stop_token().stop_possible());

	auto control = task.control();
	CHECK(control.request_cancel());
	CHECK(control.cancel_requested());
	CHECK_FALSE(control.stop_token().stop_possible());

	root::abandon_to(move(task), root::drop_on_abandon{});
	(void)src;
}
TEST_CASE(
	"work.root: no-cancellation control source yields inert stop_token",
	"[work.root]") {
	auto [control, src] = root::make_task_control_source<int>(root::SubmitOptions{.enable_cancellation = false});
	CHECK_FALSE(control.stop_token().stop_possible());
	CHECK_FALSE(src.stop_token().stop_possible());
	CHECK(control.request_cancel());
	CHECK(control.cancel_requested());
	CHECK_FALSE(control.stop_token().stop_possible());
}
TEST_CASE(
	"work.root: no-cancellation posted/operation control sources yield inert stop_token",
	"[work.root]") {
	auto [posted_control, posted_src] =
		root::make_posted_control_source<int>(root::PostOptions{.enable_cancellation = false});
	auto [operation_control, operation_src] =
		root::make_operation_control_source<int>(root::OperationOptions{.enable_cancellation = false});

	CHECK_FALSE(posted_control.stop_token().stop_possible());
	CHECK_FALSE(posted_src.stop_token().stop_possible());
	CHECK(posted_control.request_cancel());
	CHECK(posted_control.cancel_requested());
	CHECK_FALSE(posted_control.stop_token().stop_possible());

	CHECK_FALSE(operation_control.stop_token().stop_possible());
	CHECK_FALSE(operation_src.stop_token().stop_possible());
	CHECK(operation_control.request_cancel());
	CHECK(operation_control.cancel_requested());
	CHECK_FALSE(operation_control.stop_token().stop_possible());
}
TEST_CASE(
	"work.root: no-cancellation still runs installed cancel hook",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>(root::SubmitOptions{.enable_cancellation = false});
	std::atomic<bool> ran{false};
	REQUIRE(src.install_cancel_hook([&ran](root::CancelReason reason) noexcept {
		if (reason == root::CancelReason::requested) {
			ran.store(true, memory_order_release);
		}
	}));

	CHECK(task.control().request_cancel());
	CHECK(ran.load(memory_order_acquire));
	root::abandon_to(move(task), root::drop_on_abandon{});
	(void)src;
}
TEST_CASE(
	"work.root: no-cancellation posted/operation admission yields inert stop_token",
	"[work.root]") {
	OwnerCap owner{};
	DriverCap driver{};
	auto [posted, posted_src] = root::make_posted_source<int>(owner, root::PostOptions{.enable_cancellation = false});
	auto [op, op_src] = root::make_operation_source<int>(driver, root::OperationOptions{.enable_cancellation = false});

	CHECK_FALSE(posted_src.stop_token().stop_possible());
	CHECK_FALSE(op_src.stop_token().stop_possible());
	CHECK_FALSE(posted.control().stop_token().stop_possible());
	CHECK_FALSE(op.control().stop_token().stop_possible());

	root::abandon_to(move(posted), root::drop_on_abandon{});
	root::abandon_to(move(op), root::drop_on_abandon{});
	(void)posted_src;
	(void)op_src;
}
TEST_CASE(
	"work.root: no-cancellation posted/operation admission runs installed cancel hooks",
	"[work.root]") {
	OwnerCap owner{};
	DriverCap driver{};
	auto [posted, posted_src] = root::make_posted_source<int>(owner, root::PostOptions{.enable_cancellation = false});
	auto [op, op_src] = root::make_operation_source<int>(driver, root::OperationOptions{.enable_cancellation = false});
	std::atomic<int> ran{0};
	REQUIRE(posted_src.install_cancel_hook([&ran](root::CancelReason reason) noexcept {
		if (reason == root::CancelReason::requested) {
			ran.fetch_add(1, memory_order_acq_rel);
		}
	}));
	REQUIRE(op_src.install_cancel_hook([&ran](root::CancelReason reason) noexcept {
		if (reason == root::CancelReason::requested) {
			ran.fetch_add(1, memory_order_acq_rel);
		}
	}));

	CHECK(posted.control().request_cancel());
	CHECK(op.control().request_cancel());
	CHECK(ran.load(memory_order_acquire) == 2);

	root::abandon_to(move(posted), root::drop_on_abandon{});
	root::abandon_to(move(op), root::drop_on_abandon{});
	(void)posted_src;
	(void)op_src;
}
TEST_CASE(
	"work.root: no-cancellation control source still runs installed cancel hook",
	"[work.root]") {
	auto [control, src] = root::make_task_control_source<int>(root::SubmitOptions{.enable_cancellation = false});
	std::atomic<bool> ran{false};
	REQUIRE(src.install_cancel_hook([&ran](root::CancelReason reason) noexcept {
		if (reason == root::CancelReason::requested) {
			ran.store(true, memory_order_release);
		}
	}));

	CHECK(control.request_cancel());
	CHECK(ran.load(memory_order_acquire));
}
TEST_CASE(
	"work.root: no-cancellation posted/operation control sources run installed cancel hooks",
	"[work.root]") {
	auto [posted_control, posted_src] =
		root::make_posted_control_source<int>(root::PostOptions{.enable_cancellation = false});
	auto [operation_control, operation_src] =
		root::make_operation_control_source<int>(root::OperationOptions{.enable_cancellation = false});
	std::atomic<int> ran{0};
	REQUIRE(posted_src.install_cancel_hook([&ran](root::CancelReason reason) noexcept {
		if (reason == root::CancelReason::requested) {
			ran.fetch_add(1, memory_order_acq_rel);
		}
	}));
	REQUIRE(operation_src.install_cancel_hook([&ran](root::CancelReason reason) noexcept {
		if (reason == root::CancelReason::requested) {
			ran.fetch_add(1, memory_order_acq_rel);
		}
	}));

	CHECK(posted_control.request_cancel());
	CHECK(operation_control.request_cancel());
	CHECK(ran.load(memory_order_acquire) == 2);
}
TEST_CASE(
	"work.root: request_cancel returns false after terminal and for moved-from control",
	"[work.root]") {
	{
		auto [control, src] = root::make_task_control_source<int>();
		REQUIRE(src.try_set_value(root::Success<int>{1}));
		CHECK_FALSE(control.request_cancel());
	}

	{
		auto [control, src] = root::make_task_control_source<int>();
		auto moved = move(control);
		CHECK_FALSE(control.request_cancel()); // NOLINT(bugprone-use-after-move) — testing empty-handle post-move
		CHECK(moved.request_cancel());
		(void)src;
	}
}
TEST_CASE(
	"work.root: can_join enforces capability for posted",
	"[work.root]") {
	OwnerCap owner{};
	OwnerCap other{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	auto ctrl = posted.control();
	CHECK(root::can_join(owner, ctrl));
	CHECK_FALSE(root::can_join(other, ctrl));
	root::abandon_to(move(posted), root::drop_on_abandon{});
	(void)src;
}
TEST_CASE(
	"work.root: capability_id helper includes per-type tag",
	"[work.root]") {
	OuterCap const cap{};
	InnerCap const &base = cap;
	auto const base_id = root::capability_id(base);
	auto const outer_id = root::capability_id(cap);

	CHECK(base_id.address == outer_id.address);
	CHECK(base_id != outer_id);
}
TEST_CASE(
	"work.root: join throws on posted capability mismatch",
	"[work.root]") {
	OwnerCap owner{};
	OwnerCap other{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	REQUIRE(src.try_set_value(root::Success<int>{7}));
	CHECK_THROWS_AS(root::blocking_join(other, move(posted)), root::JoinError);
}
TEST_CASE(
	"work.root: operation join uses driver capability",
	"[work.root]") {
	DriverCap driver{};
	auto [op, src] = root::make_operation_source<int>(driver);
	REQUIRE(src.try_set_value(root::Success<int>{9}));
	auto val9 = root::value(driver, move(op));
	CHECK(val9 == 9);
}
TEST_CASE(
	"work.root: task join handle roundtrip",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	auto h = root::into_join_handle(move(task));
	REQUIRE(src.try_set_value(root::Success<int>{11}));
	auto val11 = root::value(move(h));
	CHECK(val11 == 11);
}
TEST_CASE(
	"work.root: posted join handle enforces capability",
	"[work.root]") {
	OwnerCap owner{};
	OwnerCap other{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	auto h = root::into_join_handle(move(posted));
	REQUIRE(src.try_set_value(root::Success<int>{5}));
	CHECK_THROWS_AS(root::blocking_join(other, move(h)), root::JoinError);
}
TEST_CASE(
	"work.root: abandon_to dispatches failure sink",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	std::atomic<bool> saw_failure{false};
	struct Sink {
		std::atomic<bool> *flag{};
		void operator ()(
			root::Failure const &) const noexcept {
			flag->store(true, memory_order_release);
		}
		void operator ()(
			root::Cancelled const &) const noexcept {}
	};
	root::abandon_to(move(task), Sink{&saw_failure});
	REQUIRE(src.try_set_exception(make_exception_ptr(std::runtime_error{"boom"})));
	CHECK(saw_failure.load(memory_order_acquire));
}
TEST_CASE(
	"work.root: late abandon_to runs sink on caller thread",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_exception(make_exception_ptr(std::runtime_error{"late"})));

	thread::id seen{};
	struct Sink {
		thread::id *seen{};
		void operator ()(
			root::Failure const &) const noexcept {
			*seen = std::this_thread::get_id();
		}
		void operator ()(
			root::Cancelled const &) const noexcept {}
	};
	auto caller_tid = std::this_thread::get_id();
	root::abandon_to(move(task), Sink{&seen});
	CHECK(seen == caller_tid);
}
TEST_CASE(
	"work.root: armed abandon_to runs sink on commit thread",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();

	mutex seen_mtx{};
	thread::id seen{};
	std::atomic<bool> done{false};
	struct Sink {
		mutex *seen_mtx{};
		thread::id *seen{};
		std::atomic<bool> *done{};
		void operator ()(
			root::Failure const &) const noexcept {
			{
				std::scoped_lock const lk{*seen_mtx};
				*seen = std::this_thread::get_id();
			}
			done->store(true, memory_order_release);
		}
		void operator ()(
			root::Cancelled const &) const noexcept {
			done->store(true, memory_order_release);
		}
	};
	root::abandon_to(move(task), Sink{&seen_mtx, &seen, &done});

	jthread const worker{
		[source = move(src)]() mutable { (void)source.try_set_exception(make_exception_ptr(std::runtime_error{"armed"})); }};
	auto worker_tid = worker.get_id();

	for (int i = 0; i < 100 && !done.load(memory_order_acquire); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds{1});
	}
	REQUIRE(done.load(memory_order_acquire));
	std::scoped_lock const lk{seen_mtx};
	CHECK(seen == worker_tid);
}
TEST_CASE(
	"work.root: Outcome<T>::value() returns success and rethrows cause directly",
	"[work.root][r3]") {
	root::Outcome<int> ok{root::Success<int>{77}};
	CHECK(ok.value() == 77);

	auto ep = make_exception_ptr(std::runtime_error{"boom"});
	root::Outcome<int> bad{root::Failure{ep}};
	CHECK_THROWS_AS(bad.value(), std::runtime_error);

	root::Outcome<int> cancelled{root::Cancelled{root::CancelReason::deadline}};
	CHECK_THROWS_AS(cancelled.value(), root::CancelledError);
}
TEST_CASE(
	"work.root: Outcome<T>::value() && supports move-only payload",
	"[work.root][r3]") {
	root::Outcome<std::unique_ptr<int>> ok{root::Success<std::unique_ptr<int>>{make_unique<int>(13)}};
	auto p = move(ok).value();
	REQUIRE(p);
	CHECK(*p == 13);
}
TEST_CASE(
	"work.root: Outcome<void>::value() returns void and throws on non-success",
	"[work.root][r3]") {
	root::Outcome<void> const ok{root::Success<void>{}};
	CHECK_NOTHROW(ok.value());

	root::Outcome<void> const bad{root::Failure{make_exception_ptr(std::runtime_error{"x"})}};
	CHECK_THROWS_AS(bad.value(), std::runtime_error);

	root::Outcome<void> const cancelled{root::Cancelled{root::CancelReason::shutdown}};
	CHECK_THROWS_AS(cancelled.value(), root::CancelledError);
}
TEST_CASE(
	"work.root: Outcome<T>::match() dispatches by branch with unwrapped value",
	"[work.root][r3]") {
	root::Outcome<int> ok{root::Success<int>{5}};
	auto r1 = move(ok).match(
		[](int &&v) { return v + 1; },
		[](root::Failure const &) { return -1; },
		[](root::Cancelled const &) { return -2; });
	CHECK(r1 == 6);

	root::Outcome<int> const bad{root::Failure{make_exception_ptr(std::runtime_error{"y"})}};
	auto r2 = bad.match(
		[](int const &v) { return v; },
		[](root::Failure const &) { return -1; },
		[](root::Cancelled const &) { return -2; });
	CHECK(r2 == -1);

	root::Outcome<int> const cancelled{root::Cancelled{root::CancelReason::requested}};
	auto r3 = cancelled.match(
		[](int const &v) { return v; },
		[](root::Failure const &) { return -1; },
		[](root::Cancelled const &) { return -2; });
	CHECK(r3 == -2);
}
TEST_CASE(
	"work.root: Outcome<void>::match() dispatches with no-arg success branch",
	"[work.root][r3]") {
	root::Outcome<void> const ok{root::Success<void>{}};
	auto r = ok.match(
		[]() { return 1; },
		[](root::Failure const &) { return 2; },
		[](root::Cancelled const &) { return 3; });
	CHECK(r == 1);
}
TEST_CASE(
	"work.root: Outcome<T>::match() && moves into success branch",
	"[work.root][r3]") {
	root::Outcome<std::unique_ptr<int>> ok{root::Success<std::unique_ptr<int>>{make_unique<int>(99)}};
	auto out = move(ok).match(
		[](std::unique_ptr<int> &&p) { return *p; },
		[](root::Failure const &) { return -1; },
		[](root::Cancelled const &) { return -2; });
	CHECK(out == 99);
}
TEST_CASE(
	"work.root: JoinError is not final — subclass compiles",
	"[work.root]") {
	struct MyJoinError : root::JoinError {
		explicit MyJoinError()
			: JoinError{root::JoinError::reason::capability_mismatch} {}
	};
	MyJoinError const err;
	CHECK(err.reason_code() == root::JoinError::reason::capability_mismatch);

	bool caught = false;
	try {
		throw MyJoinError{};
	} catch (root::JoinError const &e) {
		caught = true;
		CHECK(e.reason_code() == root::JoinError::reason::capability_mismatch);
	}
	CHECK(caught);
}
TEST_CASE(
	"work.root: R6 BasicJoinHandle operator bool is true when live, false when moved-from",
	"[work.root][r6]") {
	auto [task, src] = root::make_task_source<int>();
	auto handle = root::into_join_handle(move(task));
	REQUIRE(bool(handle));

	auto moved = move(handle);
	CHECK(!bool(handle)); // NOLINT(bugprone-use-after-move) — testing moved state
	REQUIRE(bool(moved));

	REQUIRE(src.try_set_value(root::Success<int>{7}));
	auto out = root::blocking_join(move(moved));
	CHECK(out.is_success());
}
TEST_CASE(
	"work.root: R6 default-constructed BasicJoinHandle operator bool is false",
	"[work.root][r6]") {
	root::TaskJoinHandle<int> const h;
	CHECK(!bool(h));
}
TEST_CASE(
	"work.root: R2 try_set_on_ready installs callback — fires on commit",
	"[work.root][r2]") {
	auto [task, src] = root::make_task_source<int>();
	auto handle = root::into_join_handle(move(task));
	auto ctrl = handle.control();

	std::atomic<bool> fired{false};
	auto result = ctrl.try_set_on_ready([&fired]() noexcept { fired.store(true); });
	REQUIRE(result.status == root::ReadyRegistration::installed);
	CHECK(!result.rejected_fn);
	CHECK(!fired.load());

	REQUIRE(src.try_set_value(root::Success<int>{42}));
	CHECK(fired.load());
	auto out = root::blocking_join(move(handle));
	REQUIRE(out.is_success());
	CHECK(out.value() == 42);
}
TEST_CASE(
	"work.root: R2 try_set_on_ready already_ready returns rejected_fn and fires immediately",
	"[work.root][r2]") {
	auto [task, src] = root::make_task_source<int>();
	auto handle = root::into_join_handle(move(task));
	REQUIRE(src.try_set_value(root::Success<int>{7}));

	auto ctrl = handle.control();
	std::atomic<bool> fired{false};
	auto result = ctrl.try_set_on_ready([&fired]() noexcept { fired.store(true); });
	REQUIRE(result.status == root::ReadyRegistration::already_ready);
	REQUIRE(bool(result.rejected_fn));
	CHECK(!fired.load());
	result.rejected_fn();
	CHECK(fired.load());

	auto out = root::blocking_join(move(handle));
	CHECK(out.is_success());
}
TEST_CASE(
	"work.root: R2 second try_set_on_ready returns already_installed with rejected_fn",
	"[work.root][r2]") {
	auto [task, src] = root::make_task_source<int>();
	auto handle = root::into_join_handle(move(task));
	auto ctrl = handle.control();

	std::atomic<int> count{0};
	auto r1 = ctrl.try_set_on_ready([&count]() noexcept { count.fetch_add(1); });
	REQUIRE(r1.status == root::ReadyRegistration::installed);

	auto r2 = ctrl.try_set_on_ready([&count]() noexcept { count.fetch_add(10); });
	REQUIRE(r2.status == root::ReadyRegistration::already_installed);
	REQUIRE(bool(r2.rejected_fn));

	REQUIRE(src.try_set_value(root::Success<int>{1}));
	CHECK(count.load() == 1);

	root::abandon_to(move(handle), root::drop_on_abandon{});
}
TEST_CASE(
	"work.root: co_await fails deterministically when ready callback already installed",
	"[work.root][r2]") {
	auto [task, src] = root::make_task_source<int>();
	auto ctrl = task.control();

	auto r = ctrl.try_set_on_ready([]() noexcept {});
	REQUIRE(r.status == root::ReadyRegistration::installed);

	auto outer = await_task_value(move(task));
	auto out = root::blocking_join(move(outer));
	REQUIRE(out.is_failure());
	try {
		rethrow_exception(out.failure().error);
		FAIL("expected JoinError");
	} catch (root::JoinError const &e) {
		CHECK(e.reason_code() == root::JoinError::reason::ready_callback_already_installed);
	}

	REQUIRE(src.try_set_value(root::Success<int>{5}));
}
TEST_CASE(
	"work.root: R7 clear_on_ready clears armed callback — commit fires no callback",
	"[work.root][r2][r7]") {
	auto [task, src] = root::make_task_source<int>();
	auto handle = root::into_join_handle(move(task));
	auto ctrl = handle.control();

	std::atomic<bool> fired{false};
	auto r = ctrl.try_set_on_ready([&fired]() noexcept { fired.store(true); });
	REQUIRE(r.status == root::ReadyRegistration::installed);

	auto cs = ctrl.clear_on_ready();
	REQUIRE(cs == root::ClearOnReadyStatus::cleared);

	REQUIRE(src.try_set_value(root::Success<int>{3}));
	CHECK(!fired.load());

	auto out = root::blocking_join(move(handle));
	CHECK(out.is_success());
}
TEST_CASE(
	"work.root: R7 clear_on_ready on terminal control returns already_terminal",
	"[work.root][r2][r7]") {
	auto [task, src] = root::make_task_source<int>();
	auto handle = root::into_join_handle(move(task));
	auto ctrl = handle.control();
	REQUIRE(src.try_set_value(root::Success<int>{5}));

	auto cs = ctrl.clear_on_ready();
	CHECK(cs == root::ClearOnReadyStatus::already_terminal);
	(void)root::blocking_join(move(handle));
}
TEST_CASE(
	"work.root: R7 clear_on_ready without armed hook returns not_armed",
	"[work.root][r2][r7]") {
	auto [task, src] = root::make_task_source<int>();
	auto handle = root::into_join_handle(move(task));
	auto ctrl = handle.control();

	auto cs = ctrl.clear_on_ready();
	CHECK(cs == root::ClearOnReadyStatus::not_armed);
	root::abandon_to(move(handle), root::drop_on_abandon{});
	(void)src;
}
TEST_CASE(
	"work.root: R2 set_on_ready_or_run fires inline when already ready",
	"[work.root][r2]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.try_set_value(root::Success<int>{9}));
	auto handle = root::into_join_handle(move(task));
	auto ctrl = handle.control();

	std::atomic<bool> ran{false};
	ctrl.set_on_ready_or_run([&ran]() noexcept { ran.store(true); });
	CHECK(ran.load());
	(void)root::blocking_join(move(handle));
}
TEST_CASE(
	"work.root: try_abandon_to installed returns installed status",
	"[work.root][r2]") {
	auto [task, src] = root::make_task_source<int>();
	auto handle = root::into_join_handle(move(task));

	auto status = root::try_abandon_to(move(handle), root::drop_on_abandon{});
	CHECK(status == root::AbandonStatus::installed);
	(void)src;
}
TEST_CASE(
	"work.root: try_abandon_to already_abandoned when called twice via guard",
	"[work.root][r2]") {
	auto [task, src] = root::make_task_source<int>();
	auto handle = root::into_join_handle(move(task));
	auto ctrl = handle.control();

	auto s1 = root::try_abandon_to(move(handle), root::drop_on_abandon{});
	REQUIRE(s1 == root::AbandonStatus::installed);

	root::TaskJoinHandle<int> h2;
	auto s2 = root::try_abandon_to(move(h2), root::drop_on_abandon{});
	CHECK(s2 == root::AbandonStatus::empty);
	(void)ctrl;
	(void)src;
}
// ---------------------------------------------------------------------------
// R4: joinable()
// ---------------------------------------------------------------------------

TEST_CASE(
	"work.root: joinable returns true for matching posted capability",
	"[work.root][r4]") {
	OwnerCap owner{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	auto jh = root::into_join_handle(move(posted));
	CHECK(root::joinable(owner, jh));
	root::abandon_to(move(jh), root::drop_on_abandon{});
	(void)src;
}
TEST_CASE(
	"work.root: joinable returns false for mismatched posted capability",
	"[work.root][r4]") {
	OwnerCap owner{};
	OwnerCap const other{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	auto jh = root::into_join_handle(move(posted));
	CHECK_FALSE(root::joinable(other, jh));
	root::abandon_to(move(jh), root::drop_on_abandon{});
	(void)src;
}
TEST_CASE(
	"work.root: joinable matches can_join for posted",
	"[work.root][r4]") {
	OwnerCap owner{};
	OwnerCap const other{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	auto jh = root::into_join_handle(move(posted));
	CHECK(root::joinable(owner, jh) == root::can_join(owner, jh.control()));
	CHECK(root::joinable(other, jh) == root::can_join(other, jh.control()));
	root::abandon_to(move(jh), root::drop_on_abandon{});
	(void)src;
}
TEST_CASE(
	"work.root: joinable returns true for matching operation capability",
	"[work.root][r4]") {
	DriverCap driver{};
	auto [op, src] = root::make_operation_source<int>(driver);
	auto jh = root::into_join_handle(move(op));
	CHECK(root::joinable(driver, jh));
	root::abandon_to(move(jh), root::drop_on_abandon{});
	(void)src;
}
TEST_CASE(
	"work.root: joinable returns false for mismatched operation capability",
	"[work.root][r4]") {
	DriverCap driver{};
	DriverCap const other{};
	auto [op, src] = root::make_operation_source<int>(driver);
	auto jh = root::into_join_handle(move(op));
	CHECK_FALSE(root::joinable(other, jh));
	root::abandon_to(move(jh), root::drop_on_abandon{});
	(void)src;
}
TEST_CASE(
	"work.root: joinable matches can_join for operation",
	"[work.root][r4]") {
	DriverCap driver{};
	DriverCap const other{};
	auto [op, src] = root::make_operation_source<int>(driver);
	auto jh = root::into_join_handle(move(op));
	CHECK(root::joinable(driver, jh) == root::can_join(driver, jh.control()));
	CHECK(root::joinable(other, jh) == root::can_join(other, jh.control()));
	root::abandon_to(move(jh), root::drop_on_abandon{});
	(void)src;
}
// ---------------------------------------------------------------------------
// E2b.1: work_errc / work_category
// ---------------------------------------------------------------------------

TEST_CASE(
	"work.root: work_category name is 'conflux.work'",
	"[work.root][e2b1]") {
	CHECK(std::string_view{root::work_category().name()} == "conflux.work");
}
TEST_CASE(
	"work.root: work_category is a singleton",
	"[work.root][e2b1]") {
	CHECK(&root::work_category() == &root::work_category());
}
TEST_CASE(
	"work.root: make_error_code round-trips work_errc value",
	"[work.root][e2b1]") {
	auto const ec = root::make_error_code(root::work_errc::cancelled_requested);
	CHECK(ec.value() == static_cast<int>(root::work_errc::cancelled_requested));
	CHECK(&ec.category() == &root::work_category());
}
TEST_CASE(
	"work.root: work_category message covers all enumerators",
	"[work.root][e2b1]") {
	auto const &cat = root::work_category();
	using e = root::work_errc;
	CHECK(!cat.message(static_cast<int>(e::cancelled_requested)).empty());
	CHECK(!cat.message(static_cast<int>(e::cancelled_abandoned)).empty());
	CHECK(!cat.message(static_cast<int>(e::cancelled_shutdown)).empty());
	CHECK(!cat.message(static_cast<int>(e::cancelled_deadline)).empty());
	CHECK(!cat.message(static_cast<int>(e::not_live)).empty());
	CHECK(!cat.message(static_cast<int>(e::capability_mismatch)).empty());
	CHECK(!cat.message(static_cast<int>(e::already_fulfilled)).empty());
	CHECK(!cat.message(static_cast<int>(e::already_consumed)).empty());
}
TEST_CASE(
	"work.root: cancel_reason_errc maps each CancelReason",
	"[work.root][e2b1]") {
	using enum root::CancelReason;
	CHECK(root::cancel_reason_errc(requested) == root::work_errc::cancelled_requested);
	CHECK(root::cancel_reason_errc(abandoned) == root::work_errc::cancelled_abandoned);
	CHECK(root::cancel_reason_errc(shutdown) == root::work_errc::cancelled_shutdown);
	CHECK(root::cancel_reason_errc(deadline) == root::work_errc::cancelled_deadline);
}
TEST_CASE(
	"work.root: make_error_code messages are non-empty",
	"[work.root][e2b1]") {
	using e = root::work_errc;
	for (auto code:
		 {e::cancelled_requested,
		  e::cancelled_abandoned,
		  e::cancelled_shutdown,
		  e::cancelled_deadline,
		  e::not_live,
		  e::capability_mismatch,
		  e::already_fulfilled,
		  e::already_consumed}) {
		CHECK(!root::make_error_code(code).message().empty());
	}
}
