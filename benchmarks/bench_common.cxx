module;
#include <time.h>
export module bench_common;
import std;

// ── timing ───────────────────────────────────────────────────────────────────

export [[nodiscard]] inline std::uint64_t bench_now_ns() noexcept {
	struct timespec ts{};
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL
	     + static_cast<std::uint64_t>(ts.tv_nsec);
}

// ── arg parsing ──────────────────────────────────────────────────────────────

export [[nodiscard]] inline std::size_t bench_parse_sz(char const *s) noexcept {
	std::size_t v{};
	std::from_chars(s, s + std::strlen(s), v);
	return v;
}

export struct BenchArgs {
	std::size_t iterations  = 1'000'000;
	std::size_t warmup      = 50'000;
	bool        json_out    = false;
	std::string config_name;
};

// Parses --json, --iterations, --warmup, --config-name.
// Unknown flags are silently ignored so each bench can do a second pass for
// its own extra arguments over the same argv.
export [[nodiscard]] BenchArgs bench_parse_args(std::span<char *> args) {
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
// config     — non-empty for strip1-parser benches; printed as leading NDJSON field.
// throughput — optional ops/s for human-readable display only; not in NDJSON.
export struct BenchStats {
	std::string_view config;
	std::string_view variant;
	std::size_t      iterations{};
	std::uint64_t    total_ns{};
	double           ns_per_iter{};
	double           throughput{};
};

// Prints one NDJSON line (--json) or one human-readable line.
export void bench_print(BenchStats const &s, bool json_out, bool /*first*/) {
	if (json_out) {
		if (s.config.empty())
			std::println(
				"{{\"config\":\"default\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.3f}}}",
				s.variant, s.iterations, s.total_ns, s.ns_per_iter);
		else
			std::println(
				"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.3f}}}",
				s.config, s.variant, s.iterations, s.total_ns, s.ns_per_iter);
	} else {
		if (!s.config.empty())
			std::print("[{}] ", s.config);
		if (s.throughput > 0.0)
			std::println("{:<24} {:>10} iters  {:>9.2f} ns/iter  {:>12.0f} ops/s",
			             s.variant, s.iterations, s.ns_per_iter, s.throughput);
		else
			std::println("{:<24} {:>10} iters  {:>9.2f} ns/iter",
			             s.variant, s.iterations, s.ns_per_iter);
	}
}

// ── --bench-info ─────────────────────────────────────────────────────────────

// Call at the top of main(). If argv[1] == "--bench-info", prints the JSON
// descriptor and exits 0.
export void bench_info_if_requested(int argc, char **argv, std::string_view json) {
	if (argc >= 2 && std::string_view{argv[1]} == "--bench-info") {
		std::println("{}", json);
		std::exit(0);
	}
}
