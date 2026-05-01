// task_creation_bench — measures alloc cost of make_task_source + commit + join/drop.
//
// Variants:
//   task_creation         — make_task_source<int> + commit_success + join
//   task_drop_joinable_release — make_task_source<int> + commit_success, Task dropped (not joined)
//   task_drop_joinable_debug   — same as release variant (debug/release builds differ in dtor)
//
// NDJSON output (--json): {"config":"default","variant":"...","iterations":N,"total_ns":N,"ns_per_iter":X}

#include <time.h>

import std;
import conflux.types;
import conflux.work.root;

using namespace std::string_view_literals;
namespace root = conflux::work::root;

namespace {

struct Config {
	SZ iterations = 1'000'000;
	SZ warmup = 50'000;
	bool json_out = false;
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
		}
	}
	return cfg;
}

struct Stats {
	std::string_view variant;
	SZ iterations;
	u64 total_ns;
	double ns_per_iter;
};

u64 now_ns() {
	struct timespec ts{};
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return static_cast<u64>(ts.tv_sec) * 1'000'000'000ULL + static_cast<u64>(ts.tv_nsec);
}

void run_warmup(
	SZ warmup) {
	for (SZ i = 0; i < warmup; ++i) {
		auto [task, source] = root::make_task_source<int>();
		(void)source.commit_success(root::Success<int>{42});
		(void)root::join(std::move(task));
	}
}

Stats bench_task_creation(
	SZ iters) {
	u64 const t0 = now_ns();
	for (SZ i = 0; i < iters; ++i) {
		auto [task, source] = root::make_task_source<int>();
		(void)source.commit_success(root::Success<int>{static_cast<int>(i)});
		[[maybe_unused]] auto outcome = root::join(std::move(task));
	}
	u64 const elapsed = now_ns() - t0;
	return {"task_creation", iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

Stats bench_task_drop_joinable(
	std::string_view variant_name,
	SZ iters) {
	// Until E1.x: BasicResult::~BasicResult() calls std::terminate() on a
	// joinable task, so we must join to avoid crashing. These variants will
	// measure the auto-detach dtor path once E1.x lands and diverge from
	// task_creation. For now they match task_creation numerically.
	u64 const t0 = now_ns();
	for (SZ i = 0; i < iters; ++i) {
		auto [task, source] = root::make_task_source<int>();
		(void)source.commit_success(root::Success<int>{static_cast<int>(i)});
		(void)root::join(std::move(task)); // remove after E1.x auto-detach
	}
	u64 const elapsed = now_ns() - t0;
	return {variant_name, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

void print_stats(
	Stats const &s,
	bool json_out,
	bool /*header*/) {
	if (json_out) {
		std::println(
			"{{\"config\":\"default\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.3f}}}",
			s.variant,
			s.iterations,
			s.total_ns,
			s.ns_per_iter);
	} else {
		std::println(
			"{:<40} {:>10} iters  {:>10} ns total  {:>8.2f} ns/iter",
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
			R"({"name":"task_creation","parser":"standard","configs":[{"name":"default","extra":{},"args":["--iterations","1000000","--warmup","50000"]}]})");
		return 0;
	}
	Config const cfg = parse_args(std::span{argv, static_cast<SZ>(argc)});
	run_warmup(cfg.warmup);

	Stats stats[] = {
		bench_task_creation(cfg.iterations),
		bench_task_drop_joinable("task_drop_joinable_release"sv, cfg.iterations),
		bench_task_drop_joinable("task_drop_joinable_debug"sv, cfg.iterations),
	};

	for (SZ i = 0; i < std::size(stats); ++i) {
		print_stats(stats[i], cfg.json_out, i == 0);
	}
}
