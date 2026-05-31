// task_creation_bench — measures alloc cost of make_task_source + commit + join/drop.
//
// Variants:
//   task_creation              — make_task_source<int> + try_set_value + join
//   task_drop_joinable_release — make_task_source<int> + try_set_value, Task dropped (not joined)
//   task_drop_joinable_debug   — same as release variant (debug/release builds differ in dtor)
//
// NDJSON output (--json): {"config":"","variant":"...","iterations":N,"total_ns":N,"ns_per_iter":X}

import std;
import conflux.types;
import conflux.work.root;

import bench_common;

using namespace std::string_view_literals;
namespace root = conflux::work::root;
namespace {

void run_warmup(
	std::size_t warmup) {
	for (std::size_t i = 0; i < warmup; ++i) {
		auto [task, source] = root::make_task_source<int>();
		auto set_result = source.try_set_value(root::Success<int>{42});
		auto join_result = root::blocking_join(std::move(task));
		if (!set_result) [[unlikely]] {
			std::terminate();
		}
		auto _ = join_result;
	}
}
BenchStats bench_task_creation(
	BenchSamplePlan const &plan) {
	auto stats = bench_measure_batched(
		[i = std::size_t{}]() mutable {
			auto [task, source] = root::make_task_source<int>();
			auto set_result = source.try_set_value(root::Success<int>{static_cast<int>(i)});
			++i;
			auto join_result = root::blocking_join(std::move(task));
			if (!set_result) [[unlikely]] {
				std::terminate();
			}
			auto _ = join_result;
		},
		plan);
	stats.variant = "task_creation"sv;
	return stats;
}
BenchStats bench_task_drop_joinable(
	std::string_view variant_name,
	BenchSamplePlan const &plan) {
	auto stats = bench_measure_batched(
		[i = std::size_t{}]() mutable {
			auto [task, source] = root::make_task_source<int>();
			auto set_result = source.try_set_value(root::Success<int>{static_cast<int>(i)});
			++i;
			auto join_result = root::blocking_join(std::move(task));
			if (!set_result) [[unlikely]] {
				std::terminate();
			}
			auto _ = join_result;
		},
		plan);
	stats.variant = variant_name;
	return stats;
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"task_creation","parser":"standard","configs":[{"name":"default","extra":{},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--iterations","0","--warmup","0"]}]})");

	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	auto const plan = bench_sample_plan(cfg, 200000, 40000);
	run_warmup(plan.warmup_iterations);

	BenchStats stats[] = {
		bench_task_creation(plan),
		bench_task_drop_joinable("task_drop_joinable_release"sv, plan),
		bench_task_drop_joinable("task_drop_joinable_debug"sv, plan),
	};
	for (std::size_t i = 0; i < std::size(stats); ++i) {
		bench_print(stats[i], cfg.json_out, i == 0);
	}
}
