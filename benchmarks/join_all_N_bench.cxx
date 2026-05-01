// join_all_N_bench — measures join_all fan-out with N tasks.
//
// Config JSON: { "n": N }  for N in {2, 10, 100}
// Variants:
//   join_all_N  — create N tasks, commit all immediately, join_all from a root::Task coroutine
//
// NDJSON output (--json): {"config":"...","variant":"...","iterations":N,"total_ns":N,"ns_per_iter":X}

#include <charconv>
#include <cstring>
#include <time.h>

import std;
import conflux.types;
import conflux.work;
import conflux.work.root;

using namespace std::string_view_literals;
namespace root = conflux::work::root;

namespace {

struct Config {
	SZ n = 10;
	SZ iterations = 100'000;
	SZ warmup = 5'000;
	bool json_out = false;
	std::string config_name = "n_10";
};

Config parse_args(
	std::span<char *> args) {
	Config cfg;
	for (SZ i = 1; i < args.size(); ++i) {
		std::string_view arg = args[i];
		if (arg == "--json") {
			cfg.json_out = true;
		} else if (arg == "--iterations" && i + 1 < args.size()) {
			++i;
			std::from_chars(args[i], args[i] + std::strlen(args[i]), cfg.iterations);
		} else if (arg == "--warmup" && i + 1 < args.size()) {
			++i;
			std::from_chars(args[i], args[i] + std::strlen(args[i]), cfg.warmup);
		} else if (arg == "--n" && i + 1 < args.size()) {
			++i;
			std::from_chars(args[i], args[i] + std::strlen(args[i]), cfg.n);
			cfg.config_name = std::format("n_{}", cfg.n);
		} else if (arg == "--config-name" && i + 1 < args.size()) {
			++i;
			cfg.config_name = args[i];
		}
	}
	return cfg;
}

u64 now_ns() {
	struct timespec ts{};
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return static_cast<u64>(ts.tv_sec) * 1'000'000'000ULL + static_cast<u64>(ts.tv_nsec);
}

// Synchronously creates N tasks, commits them all immediately, then joins each
// one. Uses the work-level join_all(root::Task<int>...) overload via a
// simple loop over individual joins (since join_all is variadic template,
// not a runtime-N API in the current implementation).
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

	// Commit all synchronously before joining.
	for (SZ i = 0; i < n; ++i) {
		(void)sources[i].commit_success(root::Success<int>{static_cast<int>(i)});
	}
	sources.clear(); // drop sources

	// Join all — each join is O(1) since tasks are already resolved.
	for (auto &task: tasks) {
		[[maybe_unused]] auto outcome = root::join(std::move(task));
	}
}

struct Stats {
	std::string config;
	std::string_view variant;
	SZ iterations;
	u64 total_ns;
	double ns_per_iter;
};

void print_stats(
	Stats const &s,
	bool json_out,
	bool /*header*/) {
	if (json_out) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.3f}}}",
			s.config,
			s.variant,
			s.iterations,
			s.total_ns,
			s.ns_per_iter);
	} else {
		std::println(
			"{:<20} {:<20} {:>10} iters  {:>10} ns  {:>8.2f} ns/iter",
			s.config,
			s.variant,
			s.iterations,
			s.total_ns,
			s.ns_per_iter);
	}
}

} // namespace

int main(
	int argc,
	char **argv) {
	if (argc >= 2 && std::string_view{argv[1]} == "--bench-info") {
		std::print(
			"{}\n",
			R"({"name":"join_all_N","parser":"strip1","configs":[{"name":"n_2","extra":{"n":2},"args":["--n","2","--config-name","n_2","--iterations","100000","--warmup","5000"]},{"name":"n_10","extra":{"n":10},"args":["--n","10","--config-name","n_10","--iterations","100000","--warmup","5000"]},{"name":"n_100","extra":{"n":100},"args":["--n","100","--config-name","n_100","--iterations","100000","--warmup","5000"]}]})");
		return 0;
	}
	Config const cfg = parse_args(std::span{argv, static_cast<SZ>(argc)});

	for (SZ i = 0; i < cfg.warmup; ++i) {
		run_once(cfg.n);
	}

	u64 const t0 = now_ns();
	for (SZ i = 0; i < cfg.iterations; ++i) {
		run_once(cfg.n);
	}
	u64 const elapsed = now_ns() - t0;

	Stats s{
		cfg.config_name,
		"join_all_N"sv,
		cfg.iterations,
		elapsed,
		static_cast<double>(elapsed) / static_cast<double>(cfg.iterations),
	};
	print_stats(s, cfg.json_out, true);
}
