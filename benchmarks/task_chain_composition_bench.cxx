// task_chain_composition_bench — measures N-step Chain<T> .then/.map pipeline.
//
// Config JSON: { "chain_steps": N }  for N in {1, 4, 16, 64}
// Variants:
//   chain_into_task  — N-step chain ending with into_ready_task
//
// CSV output (--json): config,variant,iterations,total_ns,ns_per_iter

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier;

import bench_common;

using namespace std::string_view_literals;
namespace root = conflux::work::root;
namespace carrier = conflux::work::carrier;
namespace {

carrier::Chain<int> apply_steps(
	carrier::Chain<int> chain,
	std::size_t n) {
	for (std::size_t i = 0; i < n; ++i) {
		chain = carrier::map(std::move(chain), [](int v) { return v + 1; });
	}
	return chain;
}
void run_once(
	std::size_t steps) {
	auto [task, source] = root::make_task_source<int>();
	(void)source.try_set_value(root::Success<int>{0});
	auto chain = carrier::from_task(std::move(task));
	chain = apply_steps(std::move(chain), steps);
	auto result_task = carrier::into_ready_task(std::move(chain));
	[[maybe_unused]] auto outcome = root::blocking_join(std::move(result_task));
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"task_chain_composition","parser":"standard","configs":[{"name":"steps_1","extra":{"chain_steps":1},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--steps","1","--config-name","steps_1","--iterations","0","--warmup","0"]},{"name":"steps_4","extra":{"chain_steps":4},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--steps","4","--config-name","steps_4","--iterations","0","--warmup","0"]},{"name":"steps_16","extra":{"chain_steps":16},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--steps","16","--config-name","steps_16","--iterations","0","--warmup","0"]},{"name":"steps_64","extra":{"chain_steps":64},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--steps","64","--config-name","steps_64","--iterations","0","--warmup","0"]}]})");

	auto cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	std::size_t chain_steps = 4;
	for (std::size_t i = 1; i < static_cast<std::size_t>(argc); ++i) {
		std::string_view a = argv[i];
		if (a == "--steps" && i + 1 < static_cast<std::size_t>(argc)) {
			chain_steps = bench_parse_sz(argv[++i]);
			if (cfg.config_name.empty()) {
				cfg.config_name = std::format("steps_{}", chain_steps);
			}
		}
	}

	auto const plan = bench_sample_plan(cfg, 200000, 40000);
	auto s = bench_measure_batched([&] { run_once(chain_steps); }, plan);

	s.config = cfg.config_name;
	s.variant = "chain_into_task"sv;
	bench_print(s, cfg.json_out, true);
}
