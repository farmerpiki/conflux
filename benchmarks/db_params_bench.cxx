import std;
import conflux.db;
import bench_common;

using namespace conflux::db;
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
	std::size_t iters,
	std::size_t warmup) {
	for (std::size_t i = 0; i < warmup; ++i) {
		Params p;
		add_n(p, n_params);
		sink.fetch_add(reinterpret_cast<std::uintptr_t>(p.values()), std::memory_order_relaxed);
	}
	std::uint64_t const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		Params p;
		add_n(p, n_params);
		sink.fetch_add(reinterpret_cast<std::uintptr_t>(p.values()), std::memory_order_relaxed);
	}
	std::uint64_t const elapsed = bench_now_ns() - t0;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(iters);
	return {cfg_name, std::string_view{"params_bind"}, iters, elapsed, ns_pi};
}

} // namespace
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"db_params","parser":"standard","configs":[{"name":"params_1","extra":{"n_params":1},"args":["--n-params","1","--config-name","params_1","--iterations","1000000","--warmup","50000"]},{"name":"params_4","extra":{"n_params":4},"args":["--n-params","4","--config-name","params_4","--iterations","1000000","--warmup","50000"]},{"name":"params_16","extra":{"n_params":16},"args":["--n-params","16","--config-name","params_16","--iterations","1000000","--warmup","50000"]},{"name":"params_64","extra":{"n_params":64},"args":["--n-params","64","--config-name","params_64","--iterations","1000000","--warmup","50000"]}]})");

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

	auto stats = bench_params(cfg.config_name, n_params, cfg.iterations, cfg.warmup);
	bench_print(stats, cfg.json_out, true);
	if (!cfg.json_out) {
		std::println("(sink={})", sink.load());
	}
}
