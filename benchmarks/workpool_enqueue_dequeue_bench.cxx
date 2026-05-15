// workpool_enqueue_dequeue_bench — measures WorkPool enqueue/dequeue throughput.
//
// Config JSON: { "threads": N }  for N in {1, 4, 16, nproc}
// Variants:
//   single_thread   — single producer, single worker, per-job blocking join
//   contended       — N producer threads, N workers, per-job blocking join
//   external_burst  — N producers enqueue a synchronized burst without per-job joins
//   local_fanout    — one worker enqueues a local-deque fanout batch
//
// NDJSON output (--json): standard timing fields plus optional queue counters.

import std;
import conflux.types;
import conflux.work;
import conflux.work.root;

import bench_common;

using namespace std::string_view_literals;
namespace root = conflux::work::root;
namespace {

struct WorkPoolBenchStats {
	BenchStats timing;
	WorkPoolQueueStats queue;
};
void print_workpool_stats(
	WorkPoolBenchStats const &s,
	bool json_out,
	bool first) {
	if (!json_out) {
		bench_print(s.timing, false, first);
		if (s.queue.enqueue_attempts != 0 || s.queue.jobs_run != 0) {
			println(
				"  queue: enqueue={} admission_contention={} local_contention={} steal_contention={} "
				"local_push={} inject_push={} jobs={} steal_hits={} futex_waits={} wake_futex={}",
				s.queue.enqueue_attempts,
				s.queue.admission_lock_contentions,
				s.queue.local_lock_contentions,
				s.queue.steal_lock_contentions,
				s.queue.local_pushes,
				s.queue.inject_pushes,
				s.queue.jobs_run,
				s.queue.steal_hits,
				s.queue.futex_waits,
				s.queue.wake_one_futex_wakes + s.queue.wake_all_futex_wakes);
		}
		return;
	}
	println(
		"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},"
		"\"ns_per_iter\":{:.2f},\"queue\":{{\"enqueue_attempts\":{},\"stopped_rejections\":{},"
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
		"\"futex_waits\":{}}}}}",
		s.timing.config,
		s.timing.variant,
		s.timing.iterations,
		s.timing.total_ns,
		s.timing.ns_per_iter,
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
		s.queue.futex_waits);
}

WorkPoolBenchStats bench_single_thread(
	SV cfg_name,
	SZ iters,
	SZ warmup) {
	WorkPool pool{WorkPoolOptions{.threads = 1}};
	auto do_iters = [&](SZ n) {
		for (SZ i = 0; i < n; ++i) {
			auto [task, source] = root::make_task_source<int>();
			auto _ = pool.enqueue([s = move(source)]() mutable { auto _ = s.try_set_value(root::Success<int>{0}); });
			[[maybe_unused]] auto outcome = root::join(move(task));
		}
	};
	do_iters(warmup);
	pool.reset_queue_stats();
	u64 const t0 = bench_now_ns();
	do_iters(iters);
	u64 const elapsed = bench_now_ns() - t0;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(iters);
	return {
		{cfg_name, "single_thread"sv, iters, elapsed, ns_pi, 1e9 / ns_pi},
		pool.queue_stats()
    };
}
WorkPoolBenchStats bench_contended(
	SV cfg_name,
	SZ threads,
	SZ iters,
	SZ warmup) {
	SZ const worker_count = max(SZ{1}, threads);
	WorkPool pool{WorkPoolOptions{.threads = worker_count}};
	SZ const per_thread = iters / threads;
	auto do_wave = [&](SZ n_per) {
		V<thread> producers;
		producers.reserve(threads);
		for (SZ t = 0; t < threads; ++t) {
			producers.emplace_back([&pool, n_per] {
				for (SZ i = 0; i < n_per; ++i) {
					auto [task, source] = root::make_task_source<int>();
					auto _ =
						pool.enqueue([s = move(source)]() mutable { auto _ = s.try_set_value(root::Success<int>{0}); });
					auto _ = root::join(move(task));
				}
			});
		}
		for (auto &th: producers) {
			th.join();
		}
	};
	do_wave(warmup / threads + 1);
	pool.reset_queue_stats();
	u64 const t0 = bench_now_ns();
	do_wave(per_thread);
	u64 const elapsed = bench_now_ns() - t0;
	SZ const total_iters = per_thread * threads;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(total_iters);
	return {
		{cfg_name, "contended"sv, total_iters, elapsed, ns_pi, 1e9 / ns_pi},
		pool.queue_stats()
    };
}
void wait_for_count(
	Atom<SZ> const &done,
	SZ expected) {
	while (done.load(memory_order_acquire) < expected) {
		std::this_thread::yield();
	}
}
void enqueue_counted_job(
	WorkPool &pool,
	Atom<SZ> &done) {
	while (!pool.enqueue([&done] { done.fetch_add(1, memory_order_release); })) {
		std::this_thread::yield();
	}
}
WorkPoolBenchStats bench_external_burst(
	SV cfg_name,
	SZ threads,
	SZ iters,
	SZ warmup) {
	SZ const worker_count = max(SZ{1}, threads);
	WorkPool pool{
		WorkPoolOptions{.threads = worker_count, .max_inject_queue = max(SZ{4096}, iters + threads + 1)}
    };
	SZ const per_thread = iters / threads;
	auto do_wave = [&](SZ n_per) {
		Atom<SZ> done{0};
		Atom<SZ> ready{0};
		Atom<bool> start{false};
		V<thread> producers;
		producers.reserve(threads);
		for (SZ t = 0; t < threads; ++t) {
			producers.emplace_back([&pool, &done, &ready, &start, n_per] {
				ready.fetch_add(1, memory_order_release);
				while (!start.load(memory_order_acquire)) {
					std::this_thread::yield();
				}
				for (SZ i = 0; i < n_per; ++i) {
					enqueue_counted_job(pool, done);
				}
			});
		}
		wait_for_count(ready, threads);
		start.store(true, memory_order_release);
		for (auto &th: producers) {
			th.join();
		}
		wait_for_count(done, n_per * threads);
	};
	do_wave(warmup / threads + 1);
	pool.reset_queue_stats();
	u64 const t0 = bench_now_ns();
	do_wave(per_thread);
	u64 const elapsed = bench_now_ns() - t0;
	SZ const total_iters = per_thread * threads;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(total_iters);
	return {
		{cfg_name, "external_burst"sv, total_iters, elapsed, ns_pi, 1e9 / ns_pi},
		pool.queue_stats()
    };
}
WorkPoolBenchStats bench_local_fanout(
	SV cfg_name,
	SZ threads,
	SZ iters,
	SZ warmup) {
	SZ const worker_count = max(SZ{1}, threads);
	WorkPool pool{
		WorkPoolOptions{
						.threads = worker_count,
						.max_inject_queue = max(SZ{4096}, threads + 1),
						.local_queue_capacity = max(SZ{1024}, iters + 1),
						}
    };
	auto do_wave = [&](SZ n) {
		Atom<SZ> done{0};
		auto [task, source] = root::make_task_source<int>();
		auto queued = pool.enqueue([&pool, &done, n, s = move(source)]() mutable {
			for (SZ i = 0; i < n; ++i) {
				enqueue_counted_job(pool, done);
			}
			auto _ = s.try_set_value(root::Success<int>{0});
		});
		if (!queued) {
			return;
		}
		auto _ = root::join(move(task));
		wait_for_count(done, n);
	};
	do_wave(warmup + 1);
	pool.reset_queue_stats();
	u64 const t0 = bench_now_ns();
	do_wave(iters);
	u64 const elapsed = bench_now_ns() - t0;
	double const ns_pi = static_cast<double>(elapsed) / static_cast<double>(iters);
	return {
		{cfg_name, "local_fanout"sv, iters, elapsed, ns_pi, 1e9 / ns_pi},
		pool.queue_stats()
    };
}

} // namespace
int main(
	int argc,
	char **argv) {
	if (argc >= 2 && SV{argv[1]} == "--bench-info") {
		auto const hw = thread::hardware_concurrency();
		V<unsigned> ts = {1u, 4u, 16u};
		if (hw != 1u && hw != 4u && hw != 16u) {
			ts.push_back(hw);
		}
		std::sort(ts.begin(), ts.end());
		S cfgs;
		for (SZ i = 0; i < ts.size(); ++i) {
			if (i > 0) {
				cfgs += ',';
			}
			cfgs += format(
				"{{\"name\":\"threads_{0}\",\"extra\":{{\"threads\":{0}}},\"args\":[\"--threads\",\"{0}\","
				"\"--config-name\",\"threads_{0}\",\"--iterations\",\"5000\",\"--warmup\",\"500\"]}}",
				ts[i]);
		}
		println("{{\"name\":\"workpool_enqueue_dequeue\",\"parser\":\"standard\",\"configs\":[{}]}}", cfgs);
		return 0;
	}

	auto cfg = bench_parse_args(span{argv, static_cast<SZ>(argc)});
	SZ threads = 1;
	for (SZ i = 1; i < static_cast<SZ>(argc); ++i) {
		SV a = argv[i];
		if (a == "--threads" && i + 1 < static_cast<SZ>(argc)) {
			threads = bench_parse_sz(argv[++i]);
			if (cfg.config_name.empty()) {
				cfg.config_name = format("threads_{}", threads);
			}
		}
	}

	threads = max(SZ{1}, threads);
	WorkPoolBenchStats stats[] = {
		bench_single_thread(cfg.config_name, cfg.iterations, cfg.warmup),
		bench_contended(cfg.config_name, threads, cfg.iterations, cfg.warmup),
		bench_external_burst(cfg.config_name, threads, cfg.iterations, cfg.warmup),
		bench_local_fanout(cfg.config_name, threads, cfg.iterations, cfg.warmup),
	};
	for (SZ i = 0; i < std::size(stats); ++i) {
		print_workpool_stats(stats[i], cfg.json_out, i == 0);
	}
}
