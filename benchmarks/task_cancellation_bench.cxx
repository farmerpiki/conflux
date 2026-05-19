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
	std::size_t iters) {
	std::uint64_t const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		auto [ctl, source] = root::make_task_control_source<int>();
		(void)ctl.request_cancel();
		(void)source.try_set_value(root::Success<int>{0});
	}
	std::uint64_t const elapsed = bench_now_ns() - t0;
	return {{}, "cancel_before_commit"sv, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}
BenchStats bench_cancel_after_commit(
	std::size_t iters) {
	std::uint64_t const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		auto [ctl, source] = root::make_task_control_source<int>();
		(void)source.try_set_value(root::Success<int>{0});
		(void)ctl.request_cancel();
	}
	std::uint64_t const elapsed = bench_now_ns() - t0;
	return {{}, "cancel_after_commit"sv, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}
BenchStats bench_cancel_with_hook(
	std::size_t iters) {
	std::atomic<std::size_t> hook_calls{0};
	std::uint64_t const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		auto [ctl, source] = root::make_task_control_source<int>();
		(void)source.install_cancel_hook(
			[&hook_calls](root::CancelReason) noexcept { hook_calls.fetch_add(1, std::memory_order_relaxed); });
		(void)ctl.request_cancel();
		(void)source.try_set_cancelled(root::work_errc::cancelled_requested);
	}
	std::uint64_t const elapsed = bench_now_ns() - t0;
	(void)hook_calls.load();
	return {{}, "cancel_with_hook"sv, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"task_cancellation","parser":"standard","configs":[{"name":"default","extra":{},"args":["--iterations","1000000","--warmup","50000"]}]})");

	auto const cfg = bench_parse_args(span{argv, static_cast<std::size_t>(argc)});

	for (std::size_t i = 0; i < cfg.warmup; ++i) {
		auto [ctl, source] = root::make_task_control_source<int>();
		(void)ctl.request_cancel();
		(void)source.try_set_cancelled(root::work_errc::cancelled_requested);
	}

	BenchStats stats[] = {
		bench_cancel_before_commit(cfg.iterations),
		bench_cancel_after_commit(cfg.iterations),
		bench_cancel_with_hook(cfg.iterations),
	};
	for (std::size_t i = 0; i < std::size(stats); ++i) {
		bench_print(stats[i], cfg.json_out, i == 0);
	}
}
