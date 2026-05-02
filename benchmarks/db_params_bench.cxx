import std;
import conflux.db;
import bench_common;

using namespace std;
using namespace conflux::db;

namespace {

atomic<uintptr_t> sink{};

void add_n(
	Params &p,
	size_t n) {
	for (size_t i = 0; i < n; ++i) {
		switch (i % 5) {
		case 0 : p.add(static_cast<int64_t>(i * 7 + 1)); break;
		case 1 : p.add(static_cast<double>(i) * 1.5); break;
		case 2 : p.add(string_view{"benchmark_string_value"}); break;
		case 3 : p.add(true); break;
		default: p.add_null(); break;
		}
	}
}

BenchStats bench_params(
	string_view cfg_name,
	size_t n_params,
	size_t iters,
	size_t warmup) {
	for (size_t i = 0; i < warmup; ++i) {
		Params p;
		add_n(p, n_params);
		sink.fetch_add(reinterpret_cast<uintptr_t>(p.values()), memory_order_relaxed);
	}
	uint64_t const t0 = bench_now_ns();
	for (size_t i = 0; i < iters; ++i) {
		Params p;
		add_n(p, n_params);
		sink.fetch_add(reinterpret_cast<uintptr_t>(p.values()), memory_order_relaxed);
	}
	uint64_t const elapsed = bench_now_ns() - t0;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(iters);
	return {cfg_name, "params_bind"sv, iters, elapsed, ns_pi};
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(argc, argv,
		R"({"name":"db_params","parser":"standard","configs":[{"name":"params_1","extra":{"n_params":1},"args":["--n-params","1","--config-name","params_1","--iterations","1000000","--warmup","50000"]},{"name":"params_4","extra":{"n_params":4},"args":["--n-params","4","--config-name","params_4","--iterations","1000000","--warmup","50000"]},{"name":"params_16","extra":{"n_params":16},"args":["--n-params","16","--config-name","params_16","--iterations","1000000","--warmup","50000"]},{"name":"params_64","extra":{"n_params":64},"args":["--n-params","64","--config-name","params_64","--iterations","1000000","--warmup","50000"]}]})");

	auto cfg = bench_parse_args(span{argv, static_cast<size_t>(argc)});
	size_t n_params = 4;
	for (size_t i = 1; i < static_cast<size_t>(argc); ++i) {
		string_view a = argv[i];
		if (a == "--n-params" && i + 1 < static_cast<size_t>(argc)) {
			n_params = bench_parse_sz(argv[++i]);
			if (cfg.config_name.empty())
				cfg.config_name = format("params_{}", n_params);
		}
	}

	auto stats = bench_params(cfg.config_name, n_params, cfg.iterations, cfg.warmup);
	bench_print(stats, cfg.json_out, true);
	if (!cfg.json_out)
		println("(sink={})", sink.load());
}
