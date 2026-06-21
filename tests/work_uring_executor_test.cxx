import std;

import conflux.work;
import conflux.work.uring_executor;
import conflux.uring;
import conflux.uring.completion;

#include <catch2/catch_test_macros.hpp>

namespace {

namespace work = conflux::work;
namespace root = conflux::work::root;

[[nodiscard]] root::Task<int> ready_int(
	int value) {
	co_return value;
}

[[nodiscard]] root::Task<int> wait_then_int(
	root::Task<void> gate,
	int value) {
	co_await std::move(gate);
	co_return value;
}

[[nodiscard]] root::Task<int> nop_then_int(
	work::UringExecutorContext &ctx,
	int value) {
	auto [task, src] = root::make_task_source<int>();
	auto shared_src = std::make_shared<root::TaskSource<int>>(std::move(src));
	auto [slot, gen] = ctx.completions().reserve([shared_src, value](conflux::uring::IoResult result) mutable {
		if (result.res < 0) {
			auto _ = shared_src->try_set_error(std::error_code{-result.res, std::generic_category()});
			return;
		}
		auto _ = shared_src->try_set_value(root::Success<int>{value});
	});
	auto sqe = ctx.ring().try_get_sqe();
	if (!sqe) {
		auto _ = shared_src->try_set_error(std::make_error_code(std::errc::resource_unavailable_try_again));
		co_return co_await std::move(task);
	}
	sqe.prep_nop().user_data(conflux::uring::UserData{ctx.encode(slot, gen)});
	co_return co_await std::move(task);
}

[[nodiscard]] std::unique_ptr<work::UringExecutor> make_executor_or_skip(
	work::UringExecutorOptions options = {}) {
	auto exec = work::try_make_uring_executor(options);
	if (!exec) {
		SKIP(std::format("io_uring executor unavailable: {}", exec.error().message()));
	}
	return std::move(*exec);
}

} // namespace

TEST_CASE(
	"work uring executor: invalid options fail before ring startup",
	"[work][uring_executor]") {
	auto exec = work::try_make_uring_executor(work::UringExecutorOptions{.max_submission_queue = 0});
	REQUIRE_FALSE(exec.has_value());
	CHECK(exec.error() == std::make_error_code(std::errc::invalid_argument));
}

TEST_CASE(
	"work uring executor: submitted task runs on owner thread",
	"[work][uring_executor]") {
	auto exec = make_executor_or_skip();
	auto caller = std::this_thread::get_id();
	auto task = exec->async_submit([caller](work::UringExecutorContext &ctx) -> root::Task<bool> {
		co_return ctx.on_owner_thread() && std::this_thread::get_id() != caller;
	});
	CHECK(work::sync_wait(std::move(task)));
}

TEST_CASE(
	"work uring executor: multi-producer submissions complete",
	"[work][uring_executor]") {
	auto exec = make_executor_or_skip();
	std::atomic<int> completed{0};
	std::array<std::jthread, 4> threads{};
	for (auto &thread: threads) {
		thread = std::jthread{[&] {
			auto task = exec->async_submit([](work::UringExecutorContext &) -> root::Task<int> {
				return ready_int(1);
			});
			completed.fetch_add(work::sync_wait(std::move(task)), std::memory_order_relaxed);
		}};
	}
	for (auto &thread: threads) {
		thread.join();
	}
	CHECK(completed.load(std::memory_order_relaxed) == 4);
}

TEST_CASE(
	"work uring executor: completion table CQE dispatch completes child task",
	"[work][uring_executor]") {
	auto exec = make_executor_or_skip();
	auto task = exec->async_submit([](work::UringExecutorContext &ctx) -> root::Task<int> {
		return nop_then_int(ctx, 42);
	});
	CHECK(work::sync_wait(std::move(task)) == 42);
}

TEST_CASE(
	"work uring executor: queue capacity rejection returns failed task",
	"[work][uring_executor]") {
	auto exec = make_executor_or_skip(work::UringExecutorOptions{.max_submission_queue = 1});
	auto [gate_task, gate_src] = root::make_task_source<void>();
	auto first = exec->async_submit([gate = std::move(gate_task)](work::UringExecutorContext &) mutable -> root::Task<int> {
		return wait_then_int(std::move(gate), 1);
	});
	auto second = exec->async_submit([](work::UringExecutorContext &) -> root::Task<int> {
		return ready_int(2);
	});
	CHECK_THROWS_AS(work::sync_wait(std::move(second)), std::system_error);
	auto _ = gate_src.try_set_value(root::Success<void>{});
	CHECK(work::sync_wait(std::move(first)) == 1);
}

TEST_CASE(
	"work uring executor: submit after stop returns cancelled task",
	"[work][uring_executor]") {
	auto exec = make_executor_or_skip();
	exec->stop();
	auto task = exec->async_submit([](work::UringExecutorContext &) -> root::Task<int> {
		return ready_int(1);
	});
	CHECK_THROWS_AS(work::sync_wait(std::move(task)), work::Cancelled);
	exec->join();
}
