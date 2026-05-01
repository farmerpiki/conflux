// join_all_N_bench — measures join_all fan-out with N tasks.
//
// Config JSON: { "n": N }  for N in {2, 10, 100}
// Variants:
//   join_all_N  — create N tasks, commit all immediately, join each
//
// CSV output (--csv): config,variant,iterations,total_ns,ns_per_iter

import std;
import conflux.types;
import conflux.work;
import conflux.work.root;

import bench_common;

using namespace std::string_view_literals;
namespace root = conflux::work::root;

namespace {

void run_once(
	SZ n) {
	std::vector<root::Task<int>> tasks;
	tasks.reserve(n);
	std::vector<root::TaskSource<int>> sources;
	sources.reserve(n);
	for (SZ i = 0; i < n; ++i) {
		auto [task, source] = root::make_task_source<int>();
		tasks.push_back(std::move(task));
		sources.push_back(std::move(source));
	}
	for (SZ i = 0; i < n; ++i) {
		(void)sources[i].try_set_value(root::Success<int>{static_cast<int>(i)});
	}
	sources.clear();
	for (auto &task: tasks) {
		[[maybe_unused]] auto outcome = root::join(std::move(task));
	}
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(argc, argv,
		R"({"name":"join_all_N","parser":"strip1","configs":[{"name":"n_2","extra":{"n":2},"args":["--n","2","--config-name","n_2","--iterations","100000","--warmup","5000"]},{"name":"n_10","extra":{"n":10},"args":["--n","10","--config-name","n_10","--iterations","100000","--warmup","5000"]},{"name":"n_100","extra":{"n":100},"args":["--n","100","--config-name","n_100","--iterations","100000","--warmup","5000"]}]})");

	auto cfg = bench_parse_args(std::span{argv, static_cast<SZ>(argc)});
	SZ n = 10;
	for (SZ i = 1; i < static_cast<SZ>(argc); ++i) {
		std::string_view a = argv[i];
		if (a == "--n" && i + 1 < static_cast<SZ>(argc)) {
			n = bench_parse_sz(argv[++i]);
			if (cfg.config_name.empty())
				cfg.config_name = std::format("n_{}", n);
		}
	}

	for (SZ i = 0; i < cfg.warmup; ++i) {
		run_once(n);
	}

	u64 const t0 = bench_now_ns();
	for (SZ i = 0; i < cfg.iterations; ++i) {
		run_once(n);
	}
	u64 const elapsed = bench_now_ns() - t0;

	BenchStats s{cfg.config_name, "join_all_N"sv, cfg.iterations, elapsed,
	             static_cast<double>(elapsed) / static_cast<double>(cfg.iterations)};
	bench_print(s, cfg.json_out, true);
}
