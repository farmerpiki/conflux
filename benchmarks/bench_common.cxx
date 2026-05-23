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
// ── arg parsing ──────────────────────────────────────────────────────────────

export [[nodiscard]] inline std::size_t bench_parse_sz(
	char const *s) noexcept {
	std::size_t v{};
	std::from_chars(s, s + std::strlen(s), v);
	return v;
}
export struct BenchArgs {
	std::size_t iterations = 200000;
	std::size_t warmup = 40000;
	bool json_out = false;
	std::string config_name;
};

export struct BenchCalibrationDefaults {
	std::size_t target_ms = 500;
	std::size_t max_iterations = 0;
	std::size_t min_iterations = 1;
	std::size_t calibration_iterations = 16;
	std::size_t min_sample_ms = 50;
};

export inline constexpr BenchCalibrationDefaults bench_calibration_defaults{};

// Parses --json, --iterations, --warmup, --config-name.
// Unknown flags are silently ignored so each bench can do a second pass for
// its own extra arguments over the same argv.
export [[nodiscard]] BenchArgs bench_parse_args(
	std::span<char *> args) {
	BenchArgs a;
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view arg = args[i];
		if (arg == "--json") {
			a.json_out = true;
		} else if (arg == "--iterations" && i + 1 < args.size()) {
			a.iterations = bench_parse_sz(args[++i]);
		} else if (arg == "--warmup" && i + 1 < args.size()) {
			a.warmup = bench_parse_sz(args[++i]);
		} else if (arg == "--config-name" && i + 1 < args.size()) {
			a.config_name = args[++i];
		}
	}
	return a;
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
	double throughput{};
};
// Prints one NDJSON line (json_out=true) or one human-readable line.
// first is accepted but unused in JSON mode (no header emitted).
export void bench_print(
	BenchStats const &s,
	bool json_out,
	bool first) {
	if (json_out) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f}}}",
			s.config,
			s.variant,
			s.iterations,
			s.total_ns,
			s.ns_per_iter);
		(void)first;
	} else {
		if (!s.config.empty()) {
			std::print("[{}] ", s.config);
		}
		if (s.throughput > 0.0) {
			std::println(
				"{:<24} {:>10} iters  {:>9.2f} ns/iter  {:>12.0f} ops/s",
				s.variant,
				s.iterations,
				s.ns_per_iter,
				s.throughput);
		} else {
			std::println("{:<24} {:>10} iters  {:>9.2f} ns/iter", s.variant, s.iterations, s.ns_per_iter);
		}
	}
}
// ── --bench-info ─────────────────────────────────────────────────────────────

// Call at the top of main(). If argv[1] == "--bench-info", prints the JSON
// descriptor and exits 0.
export void bench_info_if_requested(
	int argc,
	char **argv,
	std::string_view json) {
	if (argc >= 2 && std::string_view{argv[1]} == "--bench-info") {
		std::println("{}", json);
		std::exit(0);
	}
}
