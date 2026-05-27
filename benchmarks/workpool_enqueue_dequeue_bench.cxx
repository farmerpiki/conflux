// workpool_enqueue_dequeue_bench — measures WorkPool enqueue/dequeue throughput.
//
// Config JSON: { "threads": N }  for N in {1, 4, 16, nproc}
// Variants:
//   single_thread   — single producer, single worker, per-job blocking join
//   contended       — N producer threads, N workers, per-job blocking join
//   external_burst  — N producers enqueue a synchronized burst without per-job joins
//   local_fanout    — one worker enqueues a local-deque fanout batch
//   local_backlog_redistribution
//                  — one worker creates expensive local backlog, measuring
//                    whether peer workers help drain it
//
// When compiled with CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE, this emits a separate
// benchmark that compares both queue modes with mode-prefixed variant names.
//
// NDJSON output (--json): standard timing fields plus queue counters and
// fairness fields for redistribution profiles.

#include <conflux/detail/discard.hxx>

import std;
import conflux.types;
import conflux.work;
import conflux.work.root;

import bench_common;

using namespace std::string_view_literals;
namespace root = conflux::work::root;
namespace {

enum class BenchQueueMode : std::uint8_t {
	stealing,
	no_stealing,
};
[[nodiscard]] WorkPoolQueueMode pool_queue_mode(
	BenchQueueMode mode) noexcept {
	return mode == BenchQueueMode::no_stealing ? WorkPoolQueueMode::no_stealing : WorkPoolQueueMode::stealing;
}
[[nodiscard]] std::string_view single_thread_variant(
	BenchQueueMode mode) noexcept {
#ifdef CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE
	return mode == BenchQueueMode::no_stealing ? "no_stealing/single_thread"sv : "stealing/single_thread"sv;
#else
	(void)mode;
	return "single_thread"sv;
#endif
}
[[nodiscard]] std::string_view contended_variant(
	BenchQueueMode mode) noexcept {
#ifdef CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE
	return mode == BenchQueueMode::no_stealing ? "no_stealing/contended"sv : "stealing/contended"sv;
#else
	(void)mode;
	return "contended"sv;
#endif
}
[[nodiscard]] std::string_view external_burst_variant(
	BenchQueueMode mode) noexcept {
#ifdef CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE
	return mode == BenchQueueMode::no_stealing ? "no_stealing/external_burst"sv : "stealing/external_burst"sv;
#else
	(void)mode;
	return "external_burst"sv;
#endif
}
[[nodiscard]] std::string_view local_fanout_variant(
	BenchQueueMode mode) noexcept {
#ifdef CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE
	return mode == BenchQueueMode::no_stealing ? "no_stealing/local_fanout"sv : "stealing/local_fanout"sv;
#else
	(void)mode;
	return "local_fanout"sv;
#endif
}
[[nodiscard]] std::string_view local_backlog_redistribution_variant(
	BenchQueueMode mode) noexcept {
#ifdef CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE
	if (mode == BenchQueueMode::no_stealing) {
		return "no_stealing/local_backlog_redistribution"sv;
	}
	return "stealing/local_backlog_redistribution"sv;
#else
	(void)mode;
	return "local_backlog_redistribution"sv;
#endif
}
struct WorkPoolFairnessStats {
	std::size_t runner_threads{};
	std::size_t child_jobs{};
	std::size_t min_runner_jobs{};
	std::size_t max_runner_jobs{};
	double min_runner_share{};
	double max_runner_share{};
	std::uint64_t checksum{};
};
struct WorkPoolBenchStats {
	BenchStats timing;
	WorkPoolQueueStats queue;
	WorkPoolFairnessStats fairness{};
};
void print_workpool_stats(
	WorkPoolBenchStats const &s,
	bool json_out,
	bool first) {
	if (!json_out) {
		bench_print(s.timing, false, first);
		if (s.queue.enqueue_attempts != 0 || s.queue.jobs_run != 0) {
			std::println(
				"  queue: enqueue={} admission_contention={} local_contention={} steal_contention={} "
				"local_push={} inject_push={} jobs={} steal_hits={} futex_waits={} wake_futex={} "
				"token_discards={} token_take_failures={}",
				s.queue.enqueue_attempts,
				s.queue.admission_lock_contentions,
				s.queue.local_lock_contentions,
				s.queue.steal_lock_contentions,
				s.queue.local_pushes,
				s.queue.inject_pushes,
				s.queue.jobs_run,
				s.queue.steal_hits,
				s.queue.futex_waits,
				s.queue.wake_one_futex_wakes + s.queue.wake_all_futex_wakes,
				s.queue.queue_full_token_discards,
				s.queue.token_take_failures);
		}
		if (s.fairness.child_jobs != 0) {
			std::println(
				"  fairness: runner_threads={} child_jobs={} min_runner_jobs={} max_runner_jobs={} "
				"min_runner_share={:.3f} max_runner_share={:.3f} checksum={}",
				s.fairness.runner_threads,
				s.fairness.child_jobs,
				s.fairness.min_runner_jobs,
				s.fairness.max_runner_jobs,
				s.fairness.min_runner_share,
				s.fairness.max_runner_share,
				s.fairness.checksum);
		}
		return;
	}
	std::println(
		"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},"
		"\"ns_per_iter\":{:.2f},\"fairness\":{{\"runner_threads\":{},\"child_jobs\":{},"
		"\"min_runner_jobs\":{},\"max_runner_jobs\":{},\"min_runner_share\":{:.6f},"
		"\"max_runner_share\":{:.6f},\"checksum\":{}}},\"queue\":{{\"enqueue_attempts\":{},"
		"\"stopped_rejections\":{},"
		"\"full_rejections\":{},\"admission_lock_acquisitions\":{},"
		"\"admission_lock_contentions\":{},\"local_lock_acquisitions\":{},"
		"\"local_lock_contentions\":{},\"steal_lock_acquisitions\":{},"
		"\"steal_lock_contentions\":{},\"local_pushes\":{},"
		"\"local_push_full\":{},\"inject_pushes\":{},\"inject_push_full\":{},"
		"\"local_pop_attempts\":{},\"local_pop_hits\":{},\"inject_pop_attempts\":{},"
		"\"inject_pop_hits\":{},\"steal_rounds\":{},\"steal_victim_checks\":{},"
		"\"steal_hits\":{},\"jobs_run\":{},\"wake_one_calls\":{},"
		"\"wake_one_futex_wakes\":{},\"wake_one_elided_no_parked\":{},\"wake_all_calls\":{},"
		"\"wake_all_futex_wakes\":{},\"park_attempts\":{},\"park_recheck_skips\":{},"
		"\"futex_waits\":{},\"queue_full_token_discards\":{},\"token_take_failures\":{}}}}}",
		s.timing.config,
		s.timing.variant,
		s.timing.iterations,
		s.timing.total_ns,
		s.timing.ns_per_iter,
		s.fairness.runner_threads,
		s.fairness.child_jobs,
		s.fairness.min_runner_jobs,
		s.fairness.max_runner_jobs,
		s.fairness.min_runner_share,
		s.fairness.max_runner_share,
		s.fairness.checksum,
		s.queue.enqueue_attempts,
		s.queue.enqueue_stopped_rejections,
		s.queue.enqueue_full_rejections,
		s.queue.admission_lock_acquisitions,
		s.queue.admission_lock_contentions,
		s.queue.local_lock_acquisitions,
		s.queue.local_lock_contentions,
		s.queue.steal_lock_acquisitions,
		s.queue.steal_lock_contentions,
		s.queue.local_pushes,
		s.queue.local_push_full,
		s.queue.inject_pushes,
		s.queue.inject_push_full,
		s.queue.local_pop_attempts,
		s.queue.local_pop_hits,
		s.queue.inject_pop_attempts,
		s.queue.inject_pop_hits,
		s.queue.steal_rounds,
		s.queue.steal_victim_checks,
		s.queue.steal_hits,
		s.queue.jobs_run,
		s.queue.wake_one_calls,
		s.queue.wake_one_futex_wakes,
		s.queue.wake_one_elided_no_parked,
		s.queue.wake_all_calls,
		s.queue.wake_all_futex_wakes,
		s.queue.park_attempts,
		s.queue.park_recheck_skips,
		s.queue.futex_waits,
		s.queue.queue_full_token_discards,
		s.queue.token_take_failures);
}

WorkPoolBenchStats bench_single_thread(
	std::string_view cfg_name,
	BenchQueueMode mode,
	std::size_t iters,
	std::size_t warmup) {
	WorkPool pool{
		WorkPoolOptions{.threads = 1, .queue_mode = pool_queue_mode(mode)}
    };
	auto do_iters = [&](std::size_t n) {
		for (std::size_t i = 0; i < n; ++i) {
			auto [task, source] = root::make_task_source<int>();
			auto _ =
				pool.enqueue([s = std::move(source)]() mutable { auto _ = s.try_set_value(root::Success<int>{0}); });
			[[maybe_unused]] auto outcome = root::blocking_join(std::move(task));
		}
	};
	do_iters(warmup);
	pool.reset_queue_stats();
	std::uint64_t const t0 = bench_now_ns();
	do_iters(iters);
	std::uint64_t const elapsed = bench_now_ns() - t0;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(iters);
	return {
		{cfg_name, single_thread_variant(mode), iters, elapsed, ns_pi, 1e9 / ns_pi},
		pool.queue_stats()
    };
}
WorkPoolBenchStats bench_contended(
	std::string_view cfg_name,
	BenchQueueMode mode,
	std::size_t threads,
	std::size_t iters,
	std::size_t warmup) {
	std::size_t const worker_count = std::max(std::size_t{1}, threads);
	WorkPool pool{
		WorkPoolOptions{.threads = worker_count, .queue_mode = pool_queue_mode(mode)}
    };
	std::size_t const per_thread = iters / threads;
	auto do_wave = [&](std::size_t n_per) {
		std::vector<std::thread> producers;
		producers.reserve(threads);
		for (std::size_t t = 0; t < threads; ++t) {
			producers.emplace_back([&pool, n_per] {
				for (std::size_t i = 0; i < n_per; ++i) {
					auto [task, source] = root::make_task_source<int>();
					auto _ = pool.enqueue(
						[s = std::move(source)]() mutable { auto _ = s.try_set_value(root::Success<int>{0}); });
					CONFLUX_DISCARD(root::blocking_join(std::move(task)));
				}
			});
		}
		for (auto &th: producers) {
			th.join();
		}
	};
	do_wave(warmup / threads + 1);
	pool.reset_queue_stats();
	std::uint64_t const t0 = bench_now_ns();
	do_wave(per_thread);
	std::uint64_t const elapsed = bench_now_ns() - t0;
	std::size_t const total_iters = per_thread * threads;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(total_iters);
	return {
		{cfg_name, contended_variant(mode), total_iters, elapsed, ns_pi, 1e9 / ns_pi},
		pool.queue_stats()
    };
}
void wait_for_count(
	std::atomic<std::size_t> const &done,
	std::size_t expected) {
	while (done.load(std::memory_order_acquire) < expected) {
		std::this_thread::yield();
	}
}
void enqueue_counted_job(
	WorkPool &pool,
	std::atomic<std::size_t> &done) {
	while (!pool.enqueue([&done] { done.fetch_add(1, std::memory_order_release); })) {
		std::this_thread::yield();
	}
}
class RunnerRecorder {
	mutable std::mutex mtx_{};
	std::vector<std::pair<std::thread::id, std::size_t>> counts_{};

public:
	void note_child() {
		auto const id = std::this_thread::get_id();
		std::scoped_lock lk{mtx_};
		for (auto &[thread_id, count]: counts_) {
			if (thread_id == id) {
				++count;
				return;
			}
		}
		counts_.push_back({id, 1});
	}
	[[nodiscard]] WorkPoolFairnessStats snapshot(
		std::uint64_t checksum) const {
		std::scoped_lock lk{mtx_};
		WorkPoolFairnessStats out{.runner_threads = counts_.size(), .checksum = checksum};
		if (counts_.empty()) {
			return out;
		}
		out.min_runner_jobs = counts_.front().second;
		out.max_runner_jobs = counts_.front().second;
		for (auto const &[_, count]: counts_) {
			out.child_jobs += count;
			out.min_runner_jobs = std::min(out.min_runner_jobs, count);
			out.max_runner_jobs = std::max(out.max_runner_jobs, count);
		}
		if (out.child_jobs != 0) {
			auto const total = static_cast<double>(out.child_jobs);
			out.min_runner_share = static_cast<double>(out.min_runner_jobs) / total;
			out.max_runner_share = static_cast<double>(out.max_runner_jobs) / total;
		}
		return out;
	}
};
[[nodiscard]] std::uint64_t burn_cpu_work(
	std::size_t units,
	std::uint64_t seed) noexcept {
	auto x = seed + 0x9E3779B97F4A7C15ULL;
	for (std::size_t i = 0; i < units; ++i) {
		x ^= x >> 12U;
		x ^= x << 25U;
		x ^= x >> 27U;
		x *= 0x2545F4914F6CDD1DULL;
	}
	return x;
}
void enqueue_counted_work_job(
	WorkPool &pool,
	RunnerRecorder &recorder,
	std::atomic<std::size_t> &done,
	std::atomic<std::uint64_t> &checksum,
	std::size_t work_units,
	std::uint64_t seed) {
	while (!pool.enqueue([&recorder, &done, &checksum, work_units, seed] {
		auto const value = burn_cpu_work(work_units, seed);
		checksum.fetch_xor(value, std::memory_order_relaxed);
		recorder.note_child();
		done.fetch_add(1, std::memory_order_release);
	})) {
		std::this_thread::yield();
	}
}
WorkPoolBenchStats bench_external_burst(
	std::string_view cfg_name,
	BenchQueueMode mode,
	std::size_t threads,
	std::size_t iters,
	std::size_t warmup) {
	std::size_t const worker_count = std::max(std::size_t{1}, threads);
	WorkPool pool{
		WorkPoolOptions{
						.threads = worker_count,
						.max_inject_queue = std::max(std::size_t{4096}, iters + threads + 1),
						.queue_mode = pool_queue_mode(mode),
						}
    };
	std::size_t const per_thread = iters / threads;
	auto do_wave = [&](std::size_t n_per) {
		std::atomic<std::size_t> done{0};
		std::atomic<std::size_t> ready{0};
		std::atomic<bool> start{false};
		std::vector<std::thread> producers;
		producers.reserve(threads);
		for (std::size_t t = 0; t < threads; ++t) {
			producers.emplace_back([&pool, &done, &ready, &start, n_per] {
				ready.fetch_add(1, std::memory_order_release);
				while (!start.load(std::memory_order_acquire)) {
					std::this_thread::yield();
				}
				for (std::size_t i = 0; i < n_per; ++i) {
					enqueue_counted_job(pool, done);
				}
			});
		}
		wait_for_count(ready, threads);
		start.store(true, std::memory_order_release);
		for (auto &th: producers) {
			th.join();
		}
		wait_for_count(done, n_per * threads);
	};
	do_wave(warmup / threads + 1);
	pool.reset_queue_stats();
	std::uint64_t const t0 = bench_now_ns();
	do_wave(per_thread);
	std::uint64_t const elapsed = bench_now_ns() - t0;
	std::size_t const total_iters = per_thread * threads;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(total_iters);
	return {
		{cfg_name, external_burst_variant(mode), total_iters, elapsed, ns_pi, 1e9 / ns_pi},
		pool.queue_stats()
    };
}
WorkPoolBenchStats bench_local_fanout(
	std::string_view cfg_name,
	BenchQueueMode mode,
	std::size_t threads,
	std::size_t iters,
	std::size_t warmup) {
	std::size_t const worker_count = std::max(std::size_t{1}, threads);
	WorkPool pool{
		WorkPoolOptions{
						.threads = worker_count,
						.max_inject_queue = std::max(std::size_t{4096}, threads + 1),
						.local_queue_capacity = std::max(std::max(std::size_t{1024}, iters + 1), warmup + 2),
						.queue_mode = pool_queue_mode(mode),
						}
    };
	auto do_wave = [&](std::size_t n) {
		std::atomic<std::size_t> done{0};
		auto [task, source] = root::make_task_source<int>();
		auto queued = pool.enqueue([&pool, &done, n, s = std::move(source)]() mutable {
			for (std::size_t i = 0; i < n; ++i) {
				enqueue_counted_job(pool, done);
			}
			auto _ = s.try_set_value(root::Success<int>{0});
		});
		if (!queued) {
			return;
		}
		auto _ = root::blocking_join(std::move(task));
		wait_for_count(done, n);
	};
	do_wave(warmup + 1);
	pool.reset_queue_stats();
	std::uint64_t const t0 = bench_now_ns();
	do_wave(iters);
	std::uint64_t const elapsed = bench_now_ns() - t0;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(iters);
	return {
		{cfg_name, local_fanout_variant(mode), iters, elapsed, ns_pi, 1e9 / ns_pi},
		pool.queue_stats()
    };
}
WorkPoolBenchStats bench_local_backlog_redistribution(
	std::string_view cfg_name,
	BenchQueueMode mode,
	std::size_t threads,
	std::size_t iters,
	std::size_t warmup,
	std::size_t work_units) {
	std::size_t const worker_count = std::max(std::size_t{2}, threads);
	std::size_t const local_capacity = std::max(std::max(std::size_t{1024}, iters + 2), warmup + 2);
	WorkPool pool{
		WorkPoolOptions{
						.threads = worker_count,
						.max_inject_queue = std::max(std::size_t{4096}, worker_count + 1),
						.local_queue_capacity = local_capacity,
						.queue_mode = pool_queue_mode(mode),
						}
    };
	auto do_wave = [&](std::size_t n, bool record) -> WorkPoolFairnessStats {
		std::atomic<std::size_t> done{0};
		std::atomic<std::uint64_t> checksum{0};
		RunnerRecorder recorder{};
		auto [task, source] = root::make_task_source<int>();
		auto queued =
			pool.enqueue([&pool, &done, &checksum, &recorder, n, work_units, s = std::move(source)]() mutable {
				for (std::size_t i = 0; i < n; ++i) {
					enqueue_counted_work_job(pool, recorder, done, checksum, work_units, i + 1);
				}
				auto _ = s.try_set_value(root::Success<int>{0});
			});
		if (!queued) {
			return {};
		}
		auto _ = root::blocking_join(std::move(task));
		wait_for_count(done, n);
		if (!record) {
			return {};
		}
		return recorder.snapshot(checksum.load(std::memory_order_relaxed));
	};
	do_wave(warmup + 1, false);
	pool.reset_queue_stats();
	std::uint64_t const t0 = bench_now_ns();
	WorkPoolFairnessStats fairness = do_wave(iters, true);
	std::uint64_t const elapsed = bench_now_ns() - t0;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(iters);
	return {
		{cfg_name, local_backlog_redistribution_variant(mode), iters, elapsed, ns_pi, 1e9 / ns_pi},
		pool.queue_stats(),
		fairness
    };
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
		for (std::size_t i = 0; i < ts.size(); ++i) {
			if (i > 0) {
				cfgs += ',';
			}
			cfgs += std::format(
				"{{\"name\":\"threads_{0}\",\"extra\":{{\"threads\":{0},\"work_units\":2048"
#ifdef CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE
				",\"queue_modes\":[\"stealing\",\"no_stealing\"]"
#endif
				"}},\"target_ms\":1000,\"max_iterations\":5000,\"calibration_iterations\":2,"
				"\"args\":[\"--threads\",\"{0}\","
				"\"--config-name\",\"threads_{0}\",\"--iterations\",\"0\",\"--warmup\",\"0\","
				"\"--work\",\"2048\"]}}",
				ts[i]);
		}
		std::println(
			"{{\"name\":\"{}\",\"parser\":\"standard\",\"configs\":[{}]}}",
#ifdef CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE
			"workpool_queue_mode_compare",
#else
			"workpool_enqueue_dequeue",
#endif
			cfgs);
		return 0;
	}

	auto cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	std::size_t threads = 1;
	std::size_t work_units = 2048;
	for (std::size_t i = 1; i < static_cast<std::size_t>(argc); ++i) {
		std::string_view a = argv[i];
		if (a == "--threads" && i + 1 < static_cast<std::size_t>(argc)) {
			threads = bench_parse_sz(argv[++i]);
			if (cfg.config_name.empty()) {
				cfg.config_name = std::format("threads_{}", threads);
			}
		} else if (a == "--work" && i + 1 < static_cast<std::size_t>(argc)) {
			work_units = bench_parse_sz(argv[++i]);
		}
	}

	threads = std::max(std::size_t{1}, threads);
	work_units = std::max(std::size_t{1}, work_units);
#ifdef CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE
	WorkPoolBenchStats stats[] = {
		bench_single_thread(cfg.config_name, BenchQueueMode::stealing, cfg.iterations, cfg.warmup),
		bench_single_thread(cfg.config_name, BenchQueueMode::no_stealing, cfg.iterations, cfg.warmup),
		bench_contended(cfg.config_name, BenchQueueMode::stealing, threads, cfg.iterations, cfg.warmup),
		bench_contended(cfg.config_name, BenchQueueMode::no_stealing, threads, cfg.iterations, cfg.warmup),
		bench_external_burst(cfg.config_name, BenchQueueMode::stealing, threads, cfg.iterations, cfg.warmup),
		bench_external_burst(cfg.config_name, BenchQueueMode::no_stealing, threads, cfg.iterations, cfg.warmup),
		bench_local_fanout(cfg.config_name, BenchQueueMode::stealing, threads, cfg.iterations, cfg.warmup),
		bench_local_fanout(cfg.config_name, BenchQueueMode::no_stealing, threads, cfg.iterations, cfg.warmup),
		bench_local_backlog_redistribution(
			cfg.config_name,
			BenchQueueMode::stealing,
			threads,
			cfg.iterations,
			cfg.warmup,
			work_units),
		bench_local_backlog_redistribution(
			cfg.config_name,
			BenchQueueMode::no_stealing,
			threads,
			cfg.iterations,
			cfg.warmup,
			work_units),
	};
#else
	WorkPoolBenchStats stats[] = {
		bench_single_thread(cfg.config_name, BenchQueueMode::stealing, cfg.iterations, cfg.warmup),
		bench_contended(cfg.config_name, BenchQueueMode::stealing, threads, cfg.iterations, cfg.warmup),
		bench_external_burst(cfg.config_name, BenchQueueMode::stealing, threads, cfg.iterations, cfg.warmup),
		bench_local_fanout(cfg.config_name, BenchQueueMode::stealing, threads, cfg.iterations, cfg.warmup),
		bench_local_backlog_redistribution(
			cfg.config_name,
			BenchQueueMode::stealing,
			threads,
			cfg.iterations,
			cfg.warmup,
			work_units),
	};
#endif
	for (std::size_t i = 0; i < std::size(stats); ++i) {
		print_workpool_stats(stats[i], cfg.json_out, i == 0);
	}
}
