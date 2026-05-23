// synthetic_cqe_coro_bench — no-kernel coroutine/completion isolation rows.
//
// These rows synthesize CQE dispatch through CompletionTable and root::Task
// sources so coroutine frame allocation, continuation dispatch, task-source
// completion, and callback ownership are visible without a live io_uring trip.

import std;
import conflux.types;
import conflux.work.root;
import conflux.uring.completion;

import bench_common;

using namespace std::string_view_literals;
namespace root = conflux::work::root;

namespace {

struct DepthCase {
	std::size_t depth{};
	std::string_view config;
};

constexpr std::array<DepthCase, 5> kDepthCases{{
	{1, "depth_1"sv},
	{8, "depth_8"sv},
	{32, "depth_32"sv},
	{128, "depth_128"sv},
	{512, "depth_512"sv},
}};

struct CompletionEntry {
	std::uint32_t slot{};
	std::uint32_t gen{};
};

[[nodiscard]] root::Task<std::uint64_t> await_all(
	std::vector<root::Task<std::size_t>> tasks) {
	std::uint64_t sum = 0;
	for (auto &task: tasks) {
		sum += co_await std::move(task);
	}
	co_return sum;
}

BenchStats bench_ready_task_join(
	std::size_t iters) {
	std::uint64_t sink = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		auto [task, source] = root::make_task_source<std::size_t>();
		(void)source.try_set_value(root::Success<std::size_t>{1});
		auto outcome = root::blocking_join(std::move(task));
		if (!outcome.is_success()) {
			std::println(std::cerr, "ready_task_join did not succeed");
			std::exit(1);
		}
		sink += outcome.success().value;
	}
	auto const elapsed = bench_now_ns() - t0;
	if (sink != iters) {
		std::println(std::cerr, "bad ready_task_join sink: {}", sink);
		std::exit(1);
	}
	return {{}, "ready_task_join"sv, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

BenchStats bench_synthetic_cqe_task(
	std::size_t iters) {
	std::uint64_t sink = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		CompletionTable completions{1};
		auto [task, source] = root::make_task_source<std::size_t>();
		auto [slot, gen] = completions.reserve([source = std::move(source)](IoResult r) mutable noexcept {
			(void)source.try_set_value(root::Success<std::size_t>{static_cast<std::size_t>(r.res)});
		});
		completions.dispatch(slot, gen, 1, conflux::uring::CqeFlags{});
		auto outcome = root::blocking_join(std::move(task));
		if (!outcome.is_success()) {
			std::println(std::cerr, "synthetic_cqe_task did not succeed");
			std::exit(1);
		}
		sink += outcome.success().value;
	}
	auto const elapsed = bench_now_ns() - t0;
	if (sink != iters) {
		std::println(std::cerr, "bad synthetic_cqe_task sink: {}", sink);
		std::exit(1);
	}
	return {{}, "synthetic_cqe_task"sv, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

BenchStats bench_await_synthetic_cqes(
	std::size_t iters,
	DepthCase dc) {
	std::uint64_t sink = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t iter = 0; iter < iters; ++iter) {
		CompletionTable completions{dc.depth};
		std::vector<CompletionEntry> entries;
		std::vector<root::Task<std::size_t>> tasks;
		entries.reserve(dc.depth);
		tasks.reserve(dc.depth);
		for (std::size_t i = 0; i < dc.depth; ++i) {
			auto [task, source] = root::make_task_source<std::size_t>();
			auto [slot, gen] = completions.reserve([source = std::move(source)](IoResult r) mutable noexcept {
				(void)source.try_set_value(root::Success<std::size_t>{static_cast<std::size_t>(r.res)});
			});
			entries.push_back({.slot = slot, .gen = gen});
			tasks.push_back(std::move(task));
		}

		auto waiter = await_all(std::move(tasks));
		for (auto const &entry: entries) {
			completions.dispatch(entry.slot, entry.gen, 1, conflux::uring::CqeFlags{});
		}
		auto outcome = root::blocking_join(std::move(waiter));
		if (!outcome.is_success()) {
			std::println(std::cerr, "await_synthetic_cqes did not succeed at depth {}", dc.depth);
			std::exit(1);
		}
		sink += outcome.success().value;
	}
	auto const elapsed = bench_now_ns() - t0;
	std::size_t const ops = iters * dc.depth;
	if (sink != ops) {
		std::println(std::cerr, "bad await_synthetic_cqes sink depth {}: {} != {}", dc.depth, sink, ops);
		std::exit(1);
	}
	return {dc.config, "await_synthetic_cqes"sv, ops, elapsed, static_cast<double>(elapsed) / static_cast<double>(ops)};
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"synthetic_cqe_coro","parser":"standard","configs":[{"name":"default","extra":{"label":"micro/user-space","depths":[1,8,32,128,512]},"target_ms":500,"max_iterations":200000,"calibration_iterations":8,"args":["--iterations","0","--warmup","0"]}]})");

	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});

	if (cfg.warmup > 0) {
		(void)bench_ready_task_join(cfg.warmup);
		(void)bench_synthetic_cqe_task(cfg.warmup);
		for (auto dc: kDepthCases) {
			(void)bench_await_synthetic_cqes(cfg.warmup, dc);
		}
	}

	std::vector<BenchStats> stats;
	stats.reserve(2 + kDepthCases.size());
	stats.push_back(bench_ready_task_join(cfg.iterations));
	stats.push_back(bench_synthetic_cqe_task(cfg.iterations));
	for (auto dc: kDepthCases) {
		stats.push_back(bench_await_synthetic_cqes(cfg.iterations, dc));
	}
	for (std::size_t i = 0; i < stats.size(); ++i) {
		bench_print(stats[i], cfg.json_out, i == 0);
	}
}
