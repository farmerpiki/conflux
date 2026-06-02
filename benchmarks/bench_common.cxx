module;
#include <time.h>
export module bench_common;
import std;
import conflux.types;
// ── timing ───────────────────────────────────────────────────────────────────

export [[nodiscard]] inline std::uint64_t bench_now_ns() noexcept {
	struct timespec ts{};
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

export [[nodiscard]] inline std::uint64_t bench_clock_sample_ns() {
	static std::uint64_t cached = [] {
		constexpr std::size_t samples = 256;
		std::array<std::uint64_t, samples> deltas{};
		for (std::size_t i = 0; i < samples; ++i) {
			std::uint64_t const t0 = bench_now_ns();
			std::uint64_t const t1 = bench_now_ns();
			deltas[i] = t1 - t0;
		}
		std::ranges::sort(deltas);
		return std::max<std::uint64_t>(1, deltas[samples / 2] * 2);
	}();
	return cached;
}

export [[nodiscard]] inline std::size_t bench_ceil_div(
	std::size_t n,
	std::size_t d) noexcept {
	return d == 0 ? n : (n + d - 1) / d;
}

export struct BenchSamplePlan {
	std::size_t samples = 1;
	std::size_t batch = 1;
	std::size_t iterations = 1;
	std::size_t warmup_samples = 0;
	std::size_t warmup_iterations = 0;
	std::uint64_t timer_sample_ns = 0;
};

export [[nodiscard]] inline BenchSamplePlan bench_sample_plan(
	std::size_t iterations,
	std::size_t warmup_iterations,
	std::size_t requested_samples = 0,
	std::size_t requested_batch = 0,
	std::size_t desired_samples = 100) {
	iterations = std::max(iterations, std::size_t{1});
	desired_samples = std::max(desired_samples, std::size_t{1});

	std::size_t samples = 1;
	std::size_t batch = 1;
	if (requested_samples > 0 || requested_batch > 0) {
		batch = std::max(requested_batch, std::size_t{1});
		samples = requested_samples > 0 ? requested_samples : bench_ceil_div(iterations, batch);
		samples = std::max(samples, std::size_t{1});
		if (requested_batch == 0) {
			batch = std::max<std::size_t>(1, bench_ceil_div(iterations, samples));
		}
	} else if (iterations <= desired_samples) {
		samples = iterations;
		batch = 1;
	} else {
		samples = desired_samples;
		batch = std::max<std::size_t>(1, bench_ceil_div(iterations, samples));
	}

	std::size_t const actual_iterations = samples * batch;
	std::size_t const warmup_samples = warmup_iterations == 0 ? 0 : bench_ceil_div(warmup_iterations, batch);
	return {
		.samples = samples,
		.batch = batch,
		.iterations = actual_iterations,
		.warmup_samples = warmup_samples,
		.warmup_iterations = warmup_samples * batch,
		.timer_sample_ns = bench_clock_sample_ns(),
	};
}

export [[nodiscard]] inline double bench_timer_overhead_percent(
	BenchSamplePlan const &plan,
	std::uint64_t total_ns) noexcept {
	if (total_ns == 0 || plan.timer_sample_ns == 0) {
		return 0.0;
	}
	double const overhead = static_cast<double>(plan.samples) * static_cast<double>(plan.timer_sample_ns);
	return 100.0 * overhead / static_cast<double>(total_ns);
}

// ── arg parsing ──────────────────────────────────────────────────────────────

export [[nodiscard]] inline std::size_t bench_parse_sz(
	char const *s) {
	if (s == nullptr || *s == '\0') {
		throw std::invalid_argument{"empty benchmark size argument"};
	}
	std::size_t v{};
	auto const *last = s + std::strlen(s);
	auto const [ptr, ec] = std::from_chars(s, last, v);
	if (ec != std::errc{} || ptr != last) {
		throw std::invalid_argument{std::format("invalid benchmark size argument: {}", s)};
	}
	return v;
}
export struct BenchArgs {
	std::size_t iterations = 200000;
	std::size_t warmup = 40000;
	std::size_t samples = 0;
	std::size_t batch = 0;
	bool iterations_explicit = false;
	bool warmup_explicit = false;
	bool samples_explicit = false;
	bool batch_explicit = false;
	bool json_out = false;
	std::string config_name;
	std::vector<std::string> filters;
};

export struct BenchCalibrationDefaults {
	std::size_t target_ms = 500;
	std::size_t max_iterations = 0;
	std::size_t min_iterations = 1;
	std::size_t calibration_iterations = 16;
	std::size_t min_sample_ms = 50;
	std::size_t batch_samples = 100;
	std::size_t batch_target_ms = 5;
	std::size_t batch_timer_multiple = 300;
};

export inline constexpr BenchCalibrationDefaults bench_calibration_defaults{};

export [[nodiscard]] BenchArgs bench_parse_args(
	std::span<char *> args) {
	BenchArgs a;
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view arg = args[i];
		if (arg == "--json") {
			a.json_out = true;
		} else if (arg == "--iterations" && i + 1 < args.size()) {
			a.iterations = bench_parse_sz(args[++i]);
			a.iterations_explicit = true;
		} else if (arg == "--warmup" && i + 1 < args.size()) {
			a.warmup = bench_parse_sz(args[++i]);
			a.warmup_explicit = true;
		} else if (arg == "--samples" && i + 1 < args.size()) {
			a.samples = bench_parse_sz(args[++i]);
			a.samples_explicit = true;
		} else if (arg == "--batch" && i + 1 < args.size()) {
			a.batch = bench_parse_sz(args[++i]);
			a.batch_explicit = true;
		} else if (arg == "--config-name" && i + 1 < args.size()) {
			a.config_name = args[++i];
		} else if (arg == "--filter" && i + 1 < args.size()) {
			a.filters.emplace_back(args[++i]);
		}
	}
	return a;
}

export [[nodiscard]] inline BenchSamplePlan bench_sample_plan(
	BenchArgs const &args,
	std::size_t default_iterations,
	std::size_t default_warmup,
	std::size_t default_batch = 1,
	std::size_t desired_samples = 100) {
	std::size_t const batch_scale = std::max(default_batch, std::size_t{1});
	std::size_t const default_total_iterations = default_iterations * batch_scale;
	std::size_t const default_total_warmup = default_warmup * batch_scale;
	std::size_t const total_iterations =
		(args.iterations_explicit && args.iterations > 0) ? args.iterations : default_total_iterations;
	std::size_t const total_warmup = args.warmup_explicit ? args.warmup : default_total_warmup;
	return bench_sample_plan(total_iterations, total_warmup, args.samples, args.batch, desired_samples);
}

export [[nodiscard]] bool bench_matches_filter(
	std::span<std::string const> filters,
	std::string_view variant) {
	if (filters.empty()) {
		return true;
	}
	return std::ranges::any_of(filters, [variant](std::string const &filter) { return variant.contains(filter); });
}

export [[nodiscard]] bool bench_matches_filter(
	BenchArgs const &args,
	std::string_view variant) {
	return bench_matches_filter(std::span<std::string const>{args.filters}, variant);
}
// ── stats output ─────────────────────────────────────────────────────────────

// Standard result row.
// config     — bench config name, emitted as "config" field in NDJSON.
// throughput — optional ops/s for human-readable display only.
export struct BenchStats {
	std::string_view config;
	std::string_view variant;
	std::size_t iterations{};
	std::uint64_t total_ns{};
	double ns_per_iter{};
	double best_ns_per_iter{};
	double p10_ns_per_iter{};
	double p50_ns_per_iter{};
	double p99_ns_per_iter{};
	double throughput{};
	std::size_t sample_count{};
	std::size_t batch{};
	std::uint64_t timer_sample_ns{};
	double timer_overhead_pct{};
};

export inline void bench_apply_sample_plan(
	BenchStats &s,
	BenchSamplePlan const &plan) noexcept {
	s.sample_count = plan.samples;
	s.batch = plan.batch;
	s.timer_sample_ns = plan.timer_sample_ns;
	s.timer_overhead_pct = bench_timer_overhead_percent(plan, s.total_ns);
}

export template<typename F>
BenchStats bench_measure_batched(
	F &&fn,
	BenchSamplePlan const &plan,
	std::size_t bytes = 0) {
	for (std::size_t i = 0; i < plan.warmup_samples; ++i) {
		for (std::size_t j = 0; j < plan.batch; ++j) {
			fn();
		}
	}
	std::vector<std::uint64_t> samples;
	samples.reserve(plan.samples);
	std::uint64_t total_ns = 0;
	for (std::size_t i = 0; i < plan.samples; ++i) {
		std::uint64_t const t0 = bench_now_ns();
		for (std::size_t j = 0; j < plan.batch; ++j) {
			fn();
		}
		std::uint64_t const elapsed = bench_now_ns() - t0;
		total_ns += elapsed;
		samples.push_back(elapsed);
	}
	std::ranges::sort(samples);
	auto percentile_ns = [&](std::size_t numerator, std::size_t denominator) {
		std::size_t const last = samples.size() - 1;
		std::size_t const index = (last * numerator + denominator - 1) / denominator;
		return static_cast<double>(samples[index]) / static_cast<double>(plan.batch);
	};
	double const best_ns = static_cast<double>(samples.front()) / static_cast<double>(plan.batch);
	double const p10_ns = percentile_ns(10, 100);
	double const p50_ns = static_cast<double>(samples[plan.samples / 2]) / static_cast<double>(plan.batch);
	double const p99_ns = percentile_ns(99, 100);
	double const mbs =
		bytes > 0 && p50_ns > 0.0 ? static_cast<double>(bytes) / (p50_ns / 1e9) / (1024.0 * 1024.0) : 0.0;
	BenchStats stats{
		.iterations = plan.iterations,
		.total_ns = total_ns,
		.ns_per_iter = p50_ns,
		.best_ns_per_iter = best_ns,
		.p10_ns_per_iter = p10_ns,
		.p50_ns_per_iter = p50_ns,
		.p99_ns_per_iter = p99_ns,
		.throughput = mbs,
	};
	bench_apply_sample_plan(stats, plan);
	return stats;
}
export void bench_print(
	BenchStats const &s,
	bool json_out,
	bool first [[maybe_unused]]) {
	std::size_t const sample_count = s.sample_count == 0 ? 1 : s.sample_count;
	std::size_t const batch = s.batch == 0 ? std::max<std::size_t>(s.iterations, 1) : s.batch;
	if (json_out) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"best_ns_per_iter\":{:.2f},\"p10_ns_per_iter\":{:.2f},\"p50_ns_per_iter\":{:.2f},"
			"\"p99_ns_per_iter\":{:.2f},"
			"\"sample_count\":{},\"batch\":{},\"timer_sample_ns\":{},\"timer_overhead_pct\":{:.4f}}}",
			s.config,
			s.variant,
			s.iterations,
			s.total_ns,
			s.ns_per_iter,
			s.best_ns_per_iter,
			s.p10_ns_per_iter,
			s.p50_ns_per_iter,
			s.p99_ns_per_iter,
			sample_count,
			batch,
			s.timer_sample_ns,
			s.timer_overhead_pct);
	} else {
		if (!s.config.empty()) {
			std::print("[{}] ", s.config);
		}
		if (s.throughput > 0.0) {
			std::println(
				"{:<24} {:>10} iters  {:>9.2f} ns/iter  {:>12.0f} ops/s  [{}×{} samples, timer≈{:.2f}%]",
				s.variant,
				s.iterations,
				s.ns_per_iter,
				s.throughput,
				sample_count,
				batch,
				s.timer_overhead_pct);
		} else {
			std::println(
				"{:<24} {:>10} iters  {:>9.2f} ns/iter  [{}×{} samples, timer≈{:.2f}%]",
				s.variant,
				s.iterations,
				s.ns_per_iter,
				sample_count,
				batch,
				s.timer_overhead_pct);
		}
	}
}
// ── --bench-info ─────────────────────────────────────────────────────────────

export void bench_info_if_requested(
	int argc,
	char **argv,
	std::string_view json) {
	if (argc >= 2 && std::string_view{argv[1]} == "--bench-info") {
		std::println("{}", json);
		std::exit(0);
	}
}
