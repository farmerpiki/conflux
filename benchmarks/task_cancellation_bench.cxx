// task_cancellation_bench — measures request_cancel propagation latency.
//
// Variants:
//   cancel_before_commit  — cancel requested before source commits; join sees Cancelled
//   cancel_after_commit   — cancel requested after source commits; join sees Success (race)
//   cancel_with_hook      — cancel hook installed; measures hook dispatch overhead
//
// CSV output (--json): variant,iterations,total_ns,ns_per_iter

import std;
import conflux.types;
import conflux.work.root;

import bench_common;

using namespace std::string_view_literals;
namespace root = conflux::work::root;
namespace {

BenchStats bench_cancel_before_commit(
	BenchSamplePlan const &plan) {
	auto stats = bench_measure_batched(
		[] {
			auto [ctl, source] = root::make_task_control_source<int>();
			auto _ = ctl.request_cancel();
			_ = source.try_set_value(root::Success<int>{0});
		},
		plan);
	stats.variant = "cancel_before_commit"sv;
	return stats;
}
BenchStats bench_cancel_after_commit(
	BenchSamplePlan const &plan) {
	auto stats = bench_measure_batched(
		[] {
			auto [ctl, source] = root::make_task_control_source<int>();
			auto _ = source.try_set_value(root::Success<int>{0});
			_ = ctl.request_cancel();
		},
		plan);
	stats.variant = "cancel_after_commit"sv;
	return stats;
}
BenchStats bench_cancel_with_hook(
	BenchSamplePlan const &plan) {
	std::atomic<std::size_t> hook_calls{0};
	auto stats = bench_measure_batched(
		[&] {
			auto [ctl, source] = root::make_task_control_source<int>();
			auto _ = source.install_cancel_hook(
				[&hook_calls](root::CancelReason) noexcept { hook_calls.fetch_add(1, std::memory_order_relaxed); });
			_ = ctl.request_cancel();
			_ = source.try_set_cancelled(root::work_errc::cancelled_requested);
		},
		plan);
	auto _ = hook_calls.load();
	stats.variant = "cancel_with_hook"sv;
	return stats;
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"task_cancellation","parser":"standard","configs":[{"name":"default","extra":{},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--iterations","0","--warmup","0"]}]})");

	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	auto const plan = bench_sample_plan(cfg, 200000, 40000);

	for (std::size_t i = 0; i < plan.warmup_iterations; ++i) {
		auto [ctl, source] = root::make_task_control_source<int>();
		auto _ = ctl.request_cancel();
		_ = source.try_set_cancelled(root::work_errc::cancelled_requested);
	}

	BenchStats stats[] = {
		bench_cancel_before_commit(plan),
		bench_cancel_after_commit(plan),
		bench_cancel_with_hook(plan),
	};
	for (std::size_t i = 0; i < std::size(stats); ++i) {
		bench_print(stats[i], cfg.json_out, i == 0);
	}
}
