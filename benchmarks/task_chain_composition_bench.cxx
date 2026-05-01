// task_chain_composition_bench — measures N-step Chain<T> .then/.map pipeline.
//
// Config JSON: { "chain_steps": N }  for N in {1, 4, 16, 64}
// Variants:
//   chain_into_task  — N-step chain ending with into_ready_task
//
// NDJSON output (--json): {"config":"...","variant":"...","iterations":N,"total_ns":N,"ns_per_iter":X}

#include <charconv>
#include <cstring>
#include <time.h>

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier.model_a;

using namespace std::string_view_literals;
namespace root = conflux::work::root;
namespace model_a = conflux::work::carrier::model_a;

namespace {

struct Config {
	SZ chain_steps = 4;
	SZ iterations = 500'000;
	SZ warmup = 20'000;
	bool json_out = false;
	std::string config_name = "default";
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
		} else if (arg == "--steps" && i + 1 < args.size()) {
			++i;
			std::from_chars(args[i], args[i] + std::strlen(args[i]), cfg.chain_steps);
			cfg.config_name = std::format("steps_{}", cfg.chain_steps);
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

// Apply N .map steps to a chain, each adding 1 to the value.
model_a::Chain<int> apply_steps(
	model_a::Chain<int> chain,
	SZ n) {
	for (SZ i = 0; i < n; ++i) {
		chain = model_a::map(std::move(chain), [](int v) { return v + 1; });
	}
	return chain;
}

void run_once(
	SZ steps) {
	auto [task, source] = root::make_task_source<int>();
	(void)source.commit_success(root::Success<int>{0});
	auto chain = model_a::from_task(std::move(task));
	chain = apply_steps(std::move(chain), steps);
	auto result_task = model_a::into_ready_task(std::move(chain));
	[[maybe_unused]] auto outcome = root::join(std::move(result_task));
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
			R"({"name":"task_chain_composition","parser":"strip1","configs":[{"name":"steps_1","extra":{"chain_steps":1},"args":["--steps","1","--config-name","steps_1","--iterations","500000","--warmup","20000"]},{"name":"steps_4","extra":{"chain_steps":4},"args":["--steps","4","--config-name","steps_4","--iterations","500000","--warmup","20000"]},{"name":"steps_16","extra":{"chain_steps":16},"args":["--steps","16","--config-name","steps_16","--iterations","500000","--warmup","20000"]},{"name":"steps_64","extra":{"chain_steps":64},"args":["--steps","64","--config-name","steps_64","--iterations","500000","--warmup","20000"]}]})");
		return 0;
	}
	Config const cfg = parse_args(std::span{argv, static_cast<SZ>(argc)});

	for (SZ i = 0; i < cfg.warmup; ++i) {
		run_once(cfg.chain_steps);
	}

	u64 const t0 = now_ns();
	for (SZ i = 0; i < cfg.iterations; ++i) {
		run_once(cfg.chain_steps);
	}
	u64 const elapsed = now_ns() - t0;

	Stats s{
		cfg.config_name,
		"chain_into_task"sv,
		cfg.iterations,
		elapsed,
		static_cast<double>(elapsed) / static_cast<double>(cfg.iterations),
	};
	print_stats(s, cfg.json_out, true);
}
