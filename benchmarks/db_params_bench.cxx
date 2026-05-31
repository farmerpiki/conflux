import std;
import conflux.pg;
import bench_common;

using namespace conflux::pg;
namespace {

std::atomic<std::uintptr_t> sink{};
void add_n(
	Params &p,
	std::size_t n) {
	for (std::size_t i = 0; i < n; ++i) {
		switch (i % 5) {
		case 0 : p.add(static_cast<std::int64_t>(i * 7 + 1)); break;
		case 1 : p.add(static_cast<double>(i) * 1.5); break;
		case 2 : p.add(std::string_view{"benchmark_string_value"}); break;
		case 3 : p.add(true); break;
		default: p.add_null(); break;
		}
	}
}
BenchStats bench_params(
	std::string_view cfg_name,
	std::size_t n_params,
	BenchSamplePlan const &plan) {
	auto stats = bench_measure_batched(
		[&] {
			Params p;
			add_n(p, n_params);
			sink.fetch_add(reinterpret_cast<std::uintptr_t>(p.values()), std::memory_order_relaxed);
		},
		plan);
	stats.config = cfg_name;
	stats.variant = std::string_view{"params_bind"};
	return stats;
}

} // namespace
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"db_params","parser":"standard","configs":[{"name":"params_1","extra":{"n_params":1},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--n-params","1","--config-name","params_1","--iterations","0","--warmup","0"]},{"name":"params_4","extra":{"n_params":4},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--n-params","4","--config-name","params_4","--iterations","0","--warmup","0"]},{"name":"params_16","extra":{"n_params":16},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--n-params","16","--config-name","params_16","--iterations","0","--warmup","0"]},{"name":"params_64","extra":{"n_params":64},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--n-params","64","--config-name","params_64","--iterations","0","--warmup","0"]}]})");

	auto cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	std::size_t n_params = 4;
	for (std::size_t i = 1; i < static_cast<std::size_t>(argc); ++i) {
		std::string_view a = argv[i];
		if (a == "--n-params" && i + 1 < static_cast<std::size_t>(argc)) {
			n_params = bench_parse_sz(argv[++i]);
			if (cfg.config_name.empty()) {
				cfg.config_name = std::format("params_{}", n_params);
			}
		}
	}

	auto const plan = bench_sample_plan(cfg, 200000, 40000);
	auto stats = bench_params(cfg.config_name, n_params, plan);
	bench_print(stats, cfg.json_out, true);
	if (!cfg.json_out) {
		std::println("(sink={})", sink.load());
	}
}
