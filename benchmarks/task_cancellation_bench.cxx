// task_cancellation_bench — measures request_cancel propagation latency.
//
// Variants:
//   cancel_before_commit  — cancel requested before source commits; join sees Cancelled
//   cancel_after_commit   — cancel requested after source commits; join sees Success (race)
//   cancel_with_hook      — cancel hook installed; measures hook dispatch overhead
//
// NDJSON output (--json): {"config":"default","variant":"...","iterations":N,"total_ns":N,"ns_per_iter":X}

#include <charconv>
#include <cstring>
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

u64 now_ns() {
	struct timespec ts{};
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return static_cast<u64>(ts.tv_sec) * 1'000'000'000ULL + static_cast<u64>(ts.tv_nsec);
}

struct Stats {
	std::string_view variant;
	SZ iterations;
	u64 total_ns;
	double ns_per_iter;
};

Stats bench_cancel_before_commit(
	SZ iters) {
	u64 const t0 = now_ns();
	for (SZ i = 0; i < iters; ++i) {
		auto [ctl, source] = root::make_task_control_source<int>();
		(void)ctl.request_cancel();
		// source commits success after cancel — cancel should win
		(void)source.commit_success(root::Success<int>{0});
		// No join needed — task not observable here (no BasicResult)
		// We benchmark the cancel + commit race path.
		// Drop both — source dtor commits cancelled if not already terminal.
	}
	u64 const elapsed = now_ns() - t0;
	return {"cancel_before_commit", iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

Stats bench_cancel_after_commit(
	SZ iters) {
	u64 const t0 = now_ns();
	for (SZ i = 0; i < iters; ++i) {
		auto [ctl, source] = root::make_task_control_source<int>();
		(void)source.commit_success(root::Success<int>{0});
		(void)ctl.request_cancel();
		// Cancel arrives after success — no-op; drop both.
	}
	u64 const elapsed = now_ns() - t0;
	return {"cancel_after_commit", iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

Stats bench_cancel_with_hook(
	SZ iters) {
	std::atomic<SZ> hook_calls{0};
	u64 const t0 = now_ns();
	for (SZ i = 0; i < iters; ++i) {
		auto [ctl, source] = root::make_task_control_source<int>();
		(void)source.install_cancel_hook(
			[&hook_calls](root::CancelReason) noexcept { hook_calls.fetch_add(1, std::memory_order_relaxed); });
		(void)ctl.request_cancel();
		(void)source.commit_cancelled(root::CancelReason::requested);
	}
	u64 const elapsed = now_ns() - t0;
	(void)hook_calls.load(); // prevent optimization
	return {"cancel_with_hook", iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
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
			"{:<30} {:>10} iters  {:>10} ns  {:>8.2f} ns/iter",
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
			R"({"name":"task_cancellation","parser":"standard","configs":[{"name":"default","extra":{},"args":["--iterations","1000000","--warmup","50000"]}]})");
		return 0;
	}
	Config const cfg = parse_args(std::span{argv, static_cast<SZ>(argc)});

	// warmup
	for (SZ i = 0; i < cfg.warmup; ++i) {
		auto [ctl, source] = root::make_task_control_source<int>();
		(void)ctl.request_cancel();
		(void)source.commit_cancelled(root::CancelReason::requested);
	}

	Stats stats[] = {
		bench_cancel_before_commit(cfg.iterations),
		bench_cancel_after_commit(cfg.iterations),
		bench_cancel_with_hook(cfg.iterations),
	};

	for (SZ i = 0; i < std::size(stats); ++i) {
		print_stats(stats[i], cfg.json_out, i == 0);
	}
}
