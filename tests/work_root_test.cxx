// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.work.root;

namespace root = conflux::work::root;

namespace {

struct OwnerCap : root::capability_id_from_address<OwnerCap> {};

struct DriverCap : root::capability_id_from_address<DriverCap> {};

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
	ThrowOnCopy &operator =(
		ThrowOnCopy const &) = default;
	ThrowOnCopy(
		ThrowOnCopy &&) noexcept = default;
	ThrowOnCopy &operator =(
		ThrowOnCopy &&) noexcept = default;
};

} // namespace

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
	"work.root: Failure and FailureError normalize null exception_ptr",
	"[work.root]") {
	root::Failure f{std::exception_ptr{}};
	REQUIRE(f.error != nullptr);

	root::FailureError err{std::exception_ptr{}};
	REQUIRE(err.cause() != nullptr);
	CHECK_THROWS(err.rethrow_cause());
}

TEST_CASE(
	"work.root: task join returns committed success",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_success(root::Success<int>{42}));
	CHECK(root::value(std::move(task)) == 42);
}

TEST_CASE(
	"work.root: source destructor fallback commits abandoned cancel",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	{
		auto src_holder = std::make_optional(std::move(src));
		src_holder.reset();
	}
	auto out = root::join(std::move(task));
	REQUIRE(out.is_cancelled());
	CHECK(out.cancelled().reason == root::CancelReason::abandoned);
}

TEST_CASE(
	"work.root: guard_abandon release disarms destructor abandon",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	{
		auto guard = root::guard_abandon(std::move(task));
		task = std::move(guard).release();
	}
	REQUIRE(src.commit_success(root::Success<int>{17}));
	CHECK(root::value(std::move(task)) == 17);
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

	root::abandon_to(std::move(task), root::drop_on_abandon{});
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
	auto [posted_control, posted_src] = root::make_posted_control_source<int>(root::PostOptions{.enable_cancellation = false});
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
			ran.store(true, std::memory_order_release);
		}
	}));

	CHECK(task.control().request_cancel());
	CHECK(ran.load(std::memory_order_acquire));
	root::abandon_to(std::move(task), root::drop_on_abandon{});
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

	root::abandon_to(std::move(posted), root::drop_on_abandon{});
	root::abandon_to(std::move(op), root::drop_on_abandon{});
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
			ran.fetch_add(1, std::memory_order_acq_rel);
		}
	}));
	REQUIRE(op_src.install_cancel_hook([&ran](root::CancelReason reason) noexcept {
		if (reason == root::CancelReason::requested) {
			ran.fetch_add(1, std::memory_order_acq_rel);
		}
	}));

	CHECK(posted.control().request_cancel());
	CHECK(op.control().request_cancel());
	CHECK(ran.load(std::memory_order_acquire) == 2);

	root::abandon_to(std::move(posted), root::drop_on_abandon{});
	root::abandon_to(std::move(op), root::drop_on_abandon{});
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
			ran.store(true, std::memory_order_release);
		}
	}));

	CHECK(control.request_cancel());
	CHECK(ran.load(std::memory_order_acquire));
}

TEST_CASE(
	"work.root: no-cancellation posted/operation control sources run installed cancel hooks",
	"[work.root]") {
	auto [posted_control, posted_src] = root::make_posted_control_source<int>(root::PostOptions{.enable_cancellation = false});
	auto [operation_control, operation_src] =
		root::make_operation_control_source<int>(root::OperationOptions{.enable_cancellation = false});
	std::atomic<int> ran{0};
	REQUIRE(posted_src.install_cancel_hook([&ran](root::CancelReason reason) noexcept {
		if (reason == root::CancelReason::requested) {
			ran.fetch_add(1, std::memory_order_acq_rel);
		}
	}));
	REQUIRE(operation_src.install_cancel_hook([&ran](root::CancelReason reason) noexcept {
		if (reason == root::CancelReason::requested) {
			ran.fetch_add(1, std::memory_order_acq_rel);
		}
	}));

	CHECK(posted_control.request_cancel());
	CHECK(operation_control.request_cancel());
	CHECK(ran.load(std::memory_order_acquire) == 2);
}

TEST_CASE(
	"work.root: request_cancel returns false after terminal and for moved-from control",
	"[work.root]") {
	{
		auto [control, src] = root::make_task_control_source<int>();
		REQUIRE(src.commit_success(root::Success<int>{1}));
		CHECK_FALSE(control.request_cancel());
	}

	{
		auto [control, src] = root::make_task_control_source<int>();
		auto moved = std::move(control);
		CHECK_FALSE(control.request_cancel());
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
	root::abandon_to(std::move(posted), root::drop_on_abandon{});
	(void)src;
}

TEST_CASE(
	"work.root: capability_id helper includes per-type tag",
	"[work.root]") {
	struct InnerCap : root::capability_id_from_address<InnerCap> {};
	struct OuterCap : InnerCap, root::capability_id_from_address<OuterCap> {};

	OuterCap cap{};
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
	REQUIRE(src.commit_success(root::Success<int>{7}));
	CHECK_THROWS_AS(root::join(other, std::move(posted)), root::JoinContextError);
}

TEST_CASE(
	"work.root: operation join uses driver capability",
	"[work.root]") {
	DriverCap driver{};
	auto [op, src] = root::make_operation_source<int>(driver);
	REQUIRE(src.commit_success(root::Success<int>{9}));
	CHECK(root::value(driver, std::move(op)) == 9);
}

TEST_CASE(
	"work.root: task join handle roundtrip",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	auto h = root::into_join_handle(std::move(task));
	REQUIRE(src.commit_success(root::Success<int>{11}));
	CHECK(root::value(std::move(h)) == 11);
}

TEST_CASE(
	"work.root: posted join handle enforces capability",
	"[work.root]") {
	OwnerCap owner{};
	OwnerCap other{};
	auto [posted, src] = root::make_posted_source<int>(owner);
	auto h = root::into_join_handle(std::move(posted));
	REQUIRE(src.commit_success(root::Success<int>{5}));
	CHECK_THROWS_AS(root::join(other, std::move(h)), root::JoinContextError);
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
			flag->store(true, std::memory_order_release);
		}
		void operator ()(
			root::Cancelled const &) const noexcept {}
	};
	root::abandon_to(std::move(task), Sink{&saw_failure});
	REQUIRE(src.commit_failure(std::make_exception_ptr(std::runtime_error{"boom"})));
	CHECK(saw_failure.load(std::memory_order_acquire));
}

TEST_CASE(
	"work.root: late abandon_to runs sink on caller thread",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();
	REQUIRE(src.commit_failure(std::make_exception_ptr(std::runtime_error{"late"})));

	std::thread::id seen{};
	struct Sink {
		std::thread::id *seen{};
		void operator ()(
			root::Failure const &) const noexcept {
			*seen = std::this_thread::get_id();
		}
		void operator ()(
			root::Cancelled const &) const noexcept {}
	};

	auto caller_tid = std::this_thread::get_id();
	root::abandon_to(std::move(task), Sink{&seen});
	CHECK(seen == caller_tid);
}

TEST_CASE(
	"work.root: armed abandon_to runs sink on commit thread",
	"[work.root]") {
	auto [task, src] = root::make_task_source<int>();

	std::mutex seen_mtx{};
	std::thread::id seen{};
	std::atomic<bool> done{false};
	struct Sink {
		std::mutex *seen_mtx{};
		std::thread::id *seen{};
		std::atomic<bool> *done{};
		void operator ()(
			root::Failure const &) const noexcept {
			{
				std::scoped_lock const lk{*seen_mtx};
				*seen = std::this_thread::get_id();
			}
			done->store(true, std::memory_order_release);
		}
		void operator ()(
			root::Cancelled const &) const noexcept {
			done->store(true, std::memory_order_release);
		}
	};

	root::abandon_to(std::move(task), Sink{&seen_mtx, &seen, &done});

	std::jthread worker{[source = std::move(src)]() mutable {
		(void)source.commit_failure(std::make_exception_ptr(std::runtime_error{"armed"}));
	}};
	auto worker_tid = worker.get_id();

	for (int i = 0; i < 100 && !done.load(std::memory_order_acquire); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds{1});
	}
	REQUIRE(done.load(std::memory_order_acquire));
	std::scoped_lock const lk{seen_mtx};
	CHECK(seen == worker_tid);
}
