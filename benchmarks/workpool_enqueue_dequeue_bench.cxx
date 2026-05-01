// workpool_enqueue_dequeue_bench — measures WorkPool enqueue/dequeue throughput.
//
// Config JSON: { "threads": N }  for N in {1, 4, 16, nproc}
// Variants:
//   single_thread  — single-producer, single-worker, no cross-thread contention
//   contended      — N producer threads, WorkPool workers; N from --threads
//
// CSV output (--csv): config,variant,iterations,total_ns,ns_per_iter,throughput_ops_per_s

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
	SZ threads = 1;
	SZ iterations = 500'000;
	SZ warmup = 20'000;
	bool json_out = false;
	std::string config_name = "threads_1";
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
		} else if (arg == "--threads" && i + 1 < args.size()) {
			++i;
			std::from_chars(args[i], args[i] + std::strlen(args[i]), cfg.threads);
			cfg.config_name = std::format("threads_{}", cfg.threads);
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

struct Stats {
	std::string config;
	std::string_view variant;
	SZ iterations;
	u64 total_ns;
	double ns_per_iter;
	double throughput;
};

// Single-thread: submit tasks to pool and join from submitter thread.
Stats bench_single_thread(
	std::string const &cfg_name,
	SZ iters,
	SZ warmup) {
	WorkPool pool{WorkPoolOptions{.threads = 1}};

	auto do_iters = [&](SZ n) {
		for (SZ i = 0; i < n; ++i) {
			auto [task, source] = root::make_task_source<int>();
			pool.enqueue([s = std::move(source)]() mutable { (void)s.commit_success(root::Success<int>{0}); });
			[[maybe_unused]] auto outcome = root::join(std::move(task));
		}
	};
	do_iters(warmup);

	u64 const t0 = now_ns();
	do_iters(iters);
	u64 const elapsed = now_ns() - t0;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(iters);
	return {cfg_name, "single_thread"sv, iters, elapsed, ns_pi, 1e9 / ns_pi};
}

// Contended: N producers submit concurrently, WorkPool workers execute.
Stats bench_contended(
	std::string const &cfg_name,
	SZ threads,
	SZ iters,
	SZ warmup) {
	SZ const worker_count = std::max(SZ{1}, threads);
	WorkPool pool{WorkPoolOptions{.threads = worker_count}};
	SZ const per_thread = iters / threads;

	auto do_wave = [&](SZ n_per) {
		std::vector<std::thread> producers;
		producers.reserve(threads);
		for (SZ t = 0; t < threads; ++t) {
			producers.emplace_back([&pool, n_per] {
				for (SZ i = 0; i < n_per; ++i) {
					auto [task, source] = root::make_task_source<int>();
					pool.enqueue([s = std::move(source)]() mutable { (void)s.commit_success(root::Success<int>{0}); });
					(void)root::join(std::move(task));
				}
			});
		}
		for (auto &th: producers) {
			th.join();
		}
	};

	do_wave(warmup / threads + 1);

	u64 const t0 = now_ns();
	do_wave(per_thread);
	u64 const elapsed = now_ns() - t0;
	SZ const total_iters = per_thread * threads;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(total_iters);
	return {cfg_name, "contended"sv, total_iters, elapsed, ns_pi, 1e9 / ns_pi};
}

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
			"{:<20} {:<15} {:>10} iters  {:>8.2f} ns/iter  {:>10.0f} ops/s",
			s.config,
			s.variant,
			s.iterations,
			s.ns_per_iter,
			s.throughput);
	}
}

} // namespace

int main(
	int argc,
	char **argv) {
	if (argc >= 2 && std::string_view{argv[1]} == "--bench-info") {
		auto const hw = std::thread::hardware_concurrency();
		std::vector<unsigned> ts = {1u, 4u, 16u};
		if (hw != 1u && hw != 4u && hw != 16u) {
			ts.push_back(hw);
		}
		std::sort(ts.begin(), ts.end());
		std::string cfgs;
		for (SZ i = 0; i < ts.size(); ++i) {
			if (i > 0) {
				cfgs += ',';
			}
			cfgs += std::format(
				"{{\"name\":\"threads_{0}\",\"extra\":{{\"threads\":{0}}},\"args\":[\"--threads\",\"{0}\",\"--config-"
				"name\",\"threads_{0}\",\"--iterations\",\"500000\",\"--warmup\",\"20000\"]}}",
				ts[i]);
		}
		std::print("{{\"name\":\"workpool_enqueue_dequeue\",\"parser\":\"strip1\",\"configs\":[{}]}}\n", cfgs);
		return 0;
	}
	Config const cfg = parse_args(std::span{argv, static_cast<SZ>(argc)});

	Stats stats[] = {
		bench_single_thread(cfg.config_name, cfg.iterations, cfg.warmup),
		bench_contended(cfg.config_name, cfg.threads, cfg.iterations, cfg.warmup),
	};

	for (SZ i = 0; i < std::size(stats); ++i) {
		print_stats(stats[i], cfg.json_out, i == 0);
	}
}
