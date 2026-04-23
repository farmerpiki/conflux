#include <liburing.h>

import std;
import conflux.work;

using namespace std;

namespace {

inline atomic<size_t> sink{};

struct Config {
	bool list_only = false;
	string filter;
	optional<size_t> iterations_override;
	enum class Format : uint8_t {
		table,
		csv,
	};
	Format format = Format::table;
};

struct Stats {
	string_view name;
	size_t iterations{};
	uint64_t total_ns{};
	double ns_per_iter{};
};

using BenchFn = function<size_t()>;

struct Case {
	string_view name;
	string_view description;
	size_t default_iterations;
	BenchFn run;
};

void print_usage() {
	println("Usage: conflux_work_benchmarks [--list] [--filter SUBSTR] [--iterations N] [--format table|csv]");
}

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (size_t i = 1; i < args.size(); ++i) {
		string_view arg = args[i];
		if (arg == "--list") {
			cfg.list_only = true;
			continue;
		}
		if (arg == "--help" || arg == "-h") {
			print_usage();
			exit(0);
		}
		if (arg == "--filter") {
			if (i + 1 >= args.size()) {
				throw invalid_argument{"--filter requires a value"};
			}
			cfg.filter = args[++i];
			continue;
		}
		if (arg == "--iterations") {
			if (i + 1 >= args.size()) {
				throw invalid_argument{"--iterations requires a value"};
			}
			size_t iters = 0;
			auto const value = string_view{args[++i]};
			auto const [ptr, ec] = from_chars(value.data(), value.data() + value.size(), iters);
			if (ec != errc{} || ptr != value.data() + value.size() || iters == 0) {
				throw invalid_argument{"--iterations must be a positive integer"};
			}
			cfg.iterations_override = iters;
			continue;
		}
		if (arg == "--format") {
			if (i + 1 >= args.size()) {
				throw invalid_argument{"--format requires a value"};
			}
			auto const value = string_view{args[++i]};
			if (value == "table") {
				cfg.format = Config::Format::table;
			} else if (value == "csv") {
				cfg.format = Config::Format::csv;
			} else {
				throw invalid_argument{"--format must be table or csv"};
			}
			continue;
		}
		throw invalid_argument{format("unknown argument: {}", arg)};
	}
	return cfg;
}

bool matches_filter(
	Case const &bench,
	string_view filter) {
	return filter.empty() || bench.name.contains(filter) || bench.description.contains(filter);
}

size_t warmup_iterations(
	size_t iterations) {
	return clamp(iterations / 10, size_t{1}, size_t{1000});
}

Stats measure_case(
	Case const &bench,
	size_t iterations) {
	for (size_t i = 0; i < warmup_iterations(iterations); ++i) {
		sink.fetch_add(bench.run(), memory_order_relaxed);
	}
	auto const start = chrono::steady_clock::now();
	for (size_t i = 0; i < iterations; ++i) {
		sink.fetch_add(bench.run(), memory_order_relaxed);
	}
	auto const elapsed = chrono::steady_clock::now() - start;
	auto const total_ns = static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(elapsed).count());
	return Stats{
		.name = bench.name,
		.iterations = iterations,
		.total_ns = total_ns,
		.ns_per_iter = static_cast<double>(total_ns) / static_cast<double>(iterations)};
}

void print_header(
	Config::Format format) {
	if (format == Config::Format::table) {
		println("{:32} {:>12} {:>14} {:>14}", "Benchmark", "Iterations", "Total (ms)", "ns/iter");
	} else {
		println("name,iterations,total_ns,ns_per_iter");
	}
}

void print_stats(
	Stats const &stats,
	Config::Format format) {
	if (format == Config::Format::table) {
		auto const total_ms = static_cast<double>(stats.total_ns) / 1'000'000.0;
		println("{:32} {:>12} {:>14.3f} {:>14.1f}", stats.name, stats.iterations, total_ms, stats.ns_per_iter);
	} else {
		println("{},{},{},{}", stats.name, stats.iterations, stats.total_ns, stats.ns_per_iter);
	}
}

Case make_value_then_case() {
	return Case{
		.name = "micro/value_then",
		.description = "Immediate value through a simple then-chain",
		.default_iterations = 200'000,
		.run = [] { return static_cast<size_t>(wait(value(7) | then([](int x) { return x + 1; }))); }};
}

Case make_pool_roundtrip_case() {
	auto pool = make_shared<WorkPool>();
	return Case{
		.name = "micro/pool_roundtrip",
		.description = "Submit to WorkPool and wait for one result",
		.default_iterations = 100'000,
		.run = [pool] { return static_cast<size_t>(wait(run_on(*pool, [] { return 42; }))); }};
}

Case make_pool_chain_case() {
	auto pool = make_shared<WorkPool>();
	return Case{
		.name = "micro/pool_chain",
		.description = "WorkPool submit followed by chained continuation",
		.default_iterations = 100'000,
		.run = [pool] {
			return static_cast<size_t>(
				wait(run_on(*pool, [] { return 10; }) | then([](int x) { return x * 4; }) | then([](int x) {
						 return x + 2;
					 })));
		}};
}

Case make_join_all_case() {
	auto pool = make_shared<WorkPool>();
	return Case{
		.name = "micro/join_all",
		.description = "Join two pool tasks and one immediate value",
		.default_iterations = 50'000,
		.run = [pool] {
			auto [a, b, c] = wait(join_all(run_on(*pool, [] { return 1; }), run_on(*pool, [] { return 2; }), value(3)));
			return static_cast<size_t>(a + b + c);
		}};
}

Case make_ring_lane_case() {
	struct State {
		io_uring ring{};
		unique_ptr<RingLane> lane;

		State()
			: lane{} {
			int const rc = ::io_uring_queue_init(8, &ring, 0);
			if (rc != 0) {
				throw runtime_error{format("io_uring_queue_init failed: {}", rc)};
			}
			lane = make_unique<RingLane>(RingLaneOptions{
				.ring_fd = ring.ring_fd,
				.wake_user_data = 0x57524B42U,
				.drain_budget = 0,
				.allow_inline_on_owner = false,
			});
			lane->adopt_current_thread();
		}

		~State() { ::io_uring_queue_exit(&ring); }
	};

	auto state = make_shared<State>();
	return Case{
		.name = "micro/ring_lane_roundtrip",
		.description = "Foreign-thread enqueue, msg-ring wake, owner-thread drain",
		.default_iterations = 20'000,
		.run = [state] {
			atomic<size_t> value_out{};
			jthread producer([&] {
				bool const queued = state->lane->enqueue([&] { value_out.store(77, memory_order_release); });
				if (!queued) {
					throw runtime_error{"ring lane enqueue failed"};
				}
			});
			producer.join();
			io_uring_cqe *cqe = nullptr;
			if (::io_uring_wait_cqe(&state->ring, &cqe) != 0 || cqe == nullptr) {
				throw runtime_error{"io_uring_wait_cqe failed"};
			}
			::io_uring_cqe_seen(&state->ring, cqe);
			(void)state->lane->drain();
			return value_out.load(memory_order_acquire);
		}};
}

vector<Case> make_cases() {
	vector<Case> cases;
	cases.push_back(make_value_then_case());
	cases.push_back(make_pool_roundtrip_case());
	cases.push_back(make_pool_chain_case());
	cases.push_back(make_join_all_case());
	try {
		cases.push_back(make_ring_lane_case());
	} catch (exception const &) {}
	return cases;
}

} // namespace

int main(
	int argc,
	char **argv) {
	try {
		auto const cfg = parse_args({argv, static_cast<size_t>(argc)});
		auto cases = make_cases();
		if (cfg.list_only) {
			for (auto const &bench: cases) {
				println("{:32} {}", bench.name, bench.description);
			}
			return 0;
		}
		print_header(cfg.format);
		for (auto const &bench: cases) {
			if (!matches_filter(bench, cfg.filter)) {
				continue;
			}
			auto const iterations = cfg.iterations_override.value_or(bench.default_iterations);
			print_stats(measure_case(bench, iterations), cfg.format);
		}
		println("sink={}", sink.load(memory_order_relaxed));
		return 0;
	} catch (exception const &ex) {
		println(cerr, "conflux_work_benchmarks: {}", ex.what());
		return 1;
	}
}
