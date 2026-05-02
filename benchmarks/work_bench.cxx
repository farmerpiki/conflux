#include <liburing.h>

import std;
import conflux.types;
import conflux.work;

using namespace std::string_view_literals;

namespace root = conflux::work::root;

struct OwnerCap {};
struct DriverCap {};

namespace conflux::work::root {
template<> inline constexpr bool enable_address_capability_v<OwnerCap> = true;
template<> inline constexpr bool enable_address_capability_v<DriverCap> = true;
}

namespace {

inline Atom<SZ> sink{};

struct Config {
	bool list_only = false;
	S filter;
	Opt<SZ> iterations_override;
	enum class Format : u8 {
		table,
		json,
	};
	Format format = Format::table;
};

struct Stats {
	SV name;
	SZ iterations{};
	u64 total_ns{};
	double ns_per_iter{};
};

using BenchFn = root::detail::small_move_only_function<SZ()>;

struct Case {
	SV name;
	SV description;
	SZ default_iterations;
	BenchFn run;
};

void print_usage() {
	println("Usage: conflux_work_benchmarks [--list] [--filter SUBSTR] [--iterations N] [--format table|json]");
}

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (SZ i = 1; i < args.size(); ++i) {
		SV arg = args[i];
		if (arg == "--list") {
			cfg.list_only = true;
			continue;
		}
		if (arg == "--help" || arg == "-h") {
			print_usage();
			std::exit(0);
		}
		if (arg == "--filter") {
			if (i + 1 >= args.size()) {
				throw std::invalid_argument{"--filter requires a value"};
			}
			cfg.filter = args[++i];
			continue;
		}
		if (arg == "--iterations") {
			if (i + 1 >= args.size()) {
				throw std::invalid_argument{"--iterations requires a value"};
			}
			SZ iters = 0;
			auto const value = SV{args[++i]};
			auto const [ptr, ec] = from_chars(value.data(), value.data() + value.size(), iters);
			if (ec != errc{} || ptr != value.data() + value.size() || iters == 0) {
				throw std::invalid_argument{"--iterations must be a positive integer"};
			}
			cfg.iterations_override = iters;
			continue;
		}
		if (arg == "--format") {
			if (i + 1 >= args.size()) {
				throw std::invalid_argument{"--format requires a value"};
			}
			auto const value = SV{args[++i]};
			if (value == "table") {
				cfg.format = Config::Format::table;
			} else if (value == "json") {
				cfg.format = Config::Format::json;
			} else {
				throw std::invalid_argument{"--format must be table or json"};
			}
			continue;
		}
		if (arg == "--json") {
			cfg.format = Config::Format::json;
			continue;
		}
		throw std::invalid_argument{format("unknown argument: {}", arg)};
	}
	return cfg;
}

bool matches_filter(
	Case const &bench,
	SV filter) {
	return filter.empty() || bench.name.contains(filter) || bench.description.contains(filter);
}

SZ warmup_iterations(
	SZ iterations) {
	return std::clamp(iterations / 10, SZ{1}, SZ{1000});
}

Stats measure_case(
	Case const &bench,
	SZ iterations) {
	for (SZ i = 0; i < warmup_iterations(iterations); ++i) {
		sink.fetch_add(bench.run(), memory_order_relaxed);
	}
	auto const start = chrono::steady_clock::now();
	for (SZ i = 0; i < iterations; ++i) {
		sink.fetch_add(bench.run(), memory_order_relaxed);
	}
	auto const elapsed = chrono::steady_clock::now() - start;
	auto const total_ns = static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(elapsed).count());
	return Stats{
		.name = bench.name,
		.iterations = iterations,
		.total_ns = total_ns,
		.ns_per_iter = static_cast<double>(total_ns) / static_cast<double>(iterations)};
}

void print_header(
	Config::Format format) {
	if (format == Config::Format::table) {
		println("{:48} {:>12} {:>14} {:>14}", "Benchmark", "Iterations", "Total (ms)", "ns/iter");
	}
}

void print_stats(
	Stats const &stats,
	Config::Format format) {
	if (format == Config::Format::table) {
		auto const total_ms = static_cast<double>(stats.total_ns) / 1'000'000.0;
		println("{:48} {:>12} {:>14.3f} {:>14.1f}", stats.name, stats.iterations, total_ms, stats.ns_per_iter);
	} else {
		println("{{\"config\":\"\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f}}}",
		        stats.name, stats.iterations, stats.total_ns, stats.ns_per_iter);
	}
}

// ---------------------------------------------------------------------------
// root: lifecycle — value paths
// ---------------------------------------------------------------------------

Case make_root_task_value_case() {
	return Case{
		.name = "root/task_value",
		.description = "make_task_source + try_set_value + root::value(task)",
		.default_iterations = 200'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.try_set_value(root::Success<int>{42});
			return static_cast<SZ>(root::value(move(task)));
		}};
}

Case make_root_posted_value_case() {
	return Case{
		.name = "root/posted_value",
		.description = "make_posted_source + try_set_value + root::value(owner, posted)",
		.default_iterations = 200'000,
		.run = [] {
			OwnerCap owner{};
			auto [posted, src] = root::make_posted_source<int>(owner);
			(void)src.try_set_value(root::Success<int>{42});
			return static_cast<SZ>(root::value(owner, move(posted)));
		}};
}

Case make_root_operation_value_case() {
	return Case{
		.name = "root/operation_value",
		.description = "make_operation_source + try_set_value + root::value(driver, op)",
		.default_iterations = 200'000,
		.run = [] {
			DriverCap driver{};
			auto [op, src] = root::make_operation_source<int>(driver);
			(void)src.try_set_value(root::Success<int>{42});
			return static_cast<SZ>(root::value(driver, move(op)));
		}};
}

Case make_root_task_cancelled_case() {
	return Case{
		.name = "root/task_cancelled",
		.description = "make_task_source + try_set_cancelled + root::join (outcome check)",
		.default_iterations = 200'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.try_set_cancelled(root::CancelReason::requested);
			auto out = root::join(move(task));
			return static_cast<SZ>(out.is_cancelled() ? 1 : 0);
		}};
}

// ---------------------------------------------------------------------------
// root: lifecycle — admit/abandon paths
// ---------------------------------------------------------------------------

Case make_root_task_admit_case(
	bool enable_cancellation) {
	auto const name = enable_cancellation ? "root/task_admit_cancel_on"sv : "root/task_admit_cancel_off"sv;
	auto const desc = enable_cancellation ? "make_task_source(cancel=on) + abandon_to(drop)"sv :
	                                        "make_task_source(cancel=off) + abandon_to(drop)"sv;
	return Case{
		.name = name,
		.description = desc,
		.default_iterations = 200'000,
		.run = [enable_cancellation] {
			auto [task, src] =
				root::make_task_source<int>(root::SubmitOptions{.enable_cancellation = enable_cancellation});
			root::abandon_to(move(task), root::drop_on_abandon{});
			(void)src;
			return SZ{1};
		}};
}

Case make_root_abandon_drop_case() {
	return Case{
		.name = "root/abandon_drop",
		.description = "make_task_source + try_set_value + abandon_to(drop)",
		.default_iterations = 200'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			root::abandon_to(move(task), root::drop_on_abandon{});
			(void)src.try_set_value(root::Success<int>{1});
			return SZ{1};
		}};
}

Case make_root_abandon_sink_case() {
	struct Sink {
		SZ *seen{};
		void operator ()(
			root::Failure const &) const noexcept {}
		void operator ()(
			root::Cancelled const &) const noexcept {
			*seen = 1;
		}
	};
	return Case{
		.name = "root/abandon_sink",
		.description = "make_task_source + abandon_to(custom sink) + try_set_cancelled → sink dispatched",
		.default_iterations = 200'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			SZ seen = 0;
			root::abandon_to(move(task), Sink{.seen = &seen});
			(void)src.try_set_cancelled(root::CancelReason::requested);
			return seen;
		}};
}

// ---------------------------------------------------------------------------
// root: cancellation hooks
// ---------------------------------------------------------------------------

Case make_root_cancel_hook_case(
	bool enable_cancellation) {
	auto const name = enable_cancellation ? "root/cancel_hook_enabled"sv : "root/cancel_hook_disabled"sv;
	auto const desc = enable_cancellation ? "install_cancel_hook + request_cancel (live stop-token)"sv :
	                                        "install_cancel_hook + request_cancel (inert stop-token)"sv;
	return Case{
		.name = name,
		.description = desc,
		.default_iterations = 200'000,
		.run = [enable_cancellation] {
			auto [task, src] =
				root::make_task_source<int>(root::SubmitOptions{.enable_cancellation = enable_cancellation});
			SZ seen = 0;
			(void)src.install_cancel_hook([&seen](root::CancelReason reason) noexcept {
				if (reason == root::CancelReason::requested) {
					++seen;
				}
			});
			auto control = task.control();
			SZ score = control.request_cancel() ? 1U : 0U;
			score += seen;
			root::abandon_to(move(task), root::drop_on_abandon{});
			return score;
		}};
}

// ---------------------------------------------------------------------------
// root: callable erasure — small_move_only_function
// ---------------------------------------------------------------------------

Case make_small_fn_inline_case() {
	using Fn = root::detail::small_move_only_function<void(root::CancelReason)>;
	return Case{
		.name = "root/small_fn_inline",
		.description = "small_move_only_function: construct + move + invoke (inline fit)",
		.default_iterations = 500'000,
		.run = [] {
			SZ seen = 0;
			Fn fn{[&seen](root::CancelReason r) noexcept {
				if (r == root::CancelReason::requested) {
					++seen;
				}
			}};
			Fn moved{move(fn)};
			moved(root::CancelReason::requested);
			return seen;
		}};
}

Case make_small_fn_heap_case() {
	using Fn = root::detail::small_move_only_function<void(root::CancelReason)>;
	struct BigCapture {
		A<uintptr_t, 5> words{};
	};
	return Case{
		.name = "root/small_fn_heap",
		.description = "small_move_only_function: construct + move + invoke (heap alloc, >32B capture)",
		.default_iterations = 500'000,
		.run = [] {
			SZ seen = 0;
			BigCapture cap{};
			cap.words[0] = 0xC0FFEEU;
			Fn fn{[&seen, cap](root::CancelReason r) noexcept {
				if (r == root::CancelReason::requested) {
					seen += (cap.words[0] & 1U) + 1U;
				}
			}};
			Fn moved{move(fn)};
			moved(root::CancelReason::requested);
			return seen;
		}};
}

// ---------------------------------------------------------------------------
// work: WorkPool dispatch
// ---------------------------------------------------------------------------

Case make_pool_single_case() {
	auto pool = make_shared<WorkPool>();
	return Case{
		.name = "work/pool_single",
		.description = "run_on_task(pool, fn) + root::value(task) — single dispatch roundtrip",
		.default_iterations = 25'000,
		.run = [pool] {
			return static_cast<SZ>(root::value(run_on_task(*pool, [] { return 42; })));
		}};
}

Case make_pool_join_all_3_case() {
	auto pool = make_shared<WorkPool>();
	return Case{
		.name = "work/pool_join_all_3",
		.description = "join_all(3 × run_on_task) + root::value — 3-way fan-out",
		.default_iterations = 30'000,
		.run = [pool] {
			auto [a, b, c] = root::value(join_all(
				run_on_task(*pool, [] { return 1; }),
				run_on_task(*pool, [] { return 2; }),
				run_on_task(*pool, [] { return 3; })));
			return static_cast<SZ>(a + b + c);
		}};
}

// ---------------------------------------------------------------------------
// work: RingLane
// ---------------------------------------------------------------------------

Case make_ring_lane_case() {
	struct State {
		io_uring ring{};
		UP<RingLane> lane;

		State() {
			int const rc = ::io_uring_queue_init(8, &ring, 0);
			if (rc != 0) {
				throw RE{format("io_uring_queue_init failed: {}", rc)};
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
		.name = "work/ring_lane_roundtrip",
		.description = "RingLane enqueue from jthread + msg-ring wake + owner drain",
		.default_iterations = 5'000,
		.run = [state] {
			Atom<SZ> out{};
			jthread producer([&] {
				if (!state->lane->enqueue([&out] { out.store(77, memory_order_release); })) {
					throw RE{"ring lane enqueue failed"};
				}
			});
			producer.join();
			io_uring_cqe *cqe = nullptr;
			if (::io_uring_wait_cqe(&state->ring, &cqe) != 0 || cqe == nullptr) {
				throw RE{"io_uring_wait_cqe failed"};
			}
			::io_uring_cqe_seen(&state->ring, cqe);
			(void)state->lane->drain();
			return out.load(memory_order_acquire);
		}};
}


V<Case> make_cases() {
	V<Case> cases;
	// root: lifecycle — value paths (sync join)
	cases.push_back(make_root_task_value_case());
	cases.push_back(make_root_posted_value_case());
	cases.push_back(make_root_operation_value_case());
	cases.push_back(make_root_task_cancelled_case());
	// root: lifecycle — admit/abandon
	cases.push_back(make_root_task_admit_case(false));
	cases.push_back(make_root_task_admit_case(true));
	cases.push_back(make_root_abandon_drop_case());
	cases.push_back(make_root_abandon_sink_case());
	// root: cancellation hooks
	cases.push_back(make_root_cancel_hook_case(true));
	cases.push_back(make_root_cancel_hook_case(false));
	// root: callable erasure
	cases.push_back(make_small_fn_inline_case());
	cases.push_back(make_small_fn_heap_case());
	// work: pool dispatch (sync join)
	cases.push_back(make_pool_single_case());
	cases.push_back(make_pool_join_all_3_case());
	// work: ring lane
	try {
		cases.push_back(make_ring_lane_case());
	} catch (exception const &) {}
	return cases;
}

} // namespace

int main(
	int argc,
	char **argv) {
	if (argc >= 2 && SV{argv[1]} == "--bench-info") {
		std::print(
			"{}\n",
			R"({"name":"work","parser":"standard","configs":[{"name":"default","extra":{},"args":[]}]})");
		return 0;
	}
	try {
		auto const cfg = parse_args({argv, static_cast<SZ>(argc)});
		auto cases = make_cases();
		if (cfg.list_only) {
			for (auto const &bench: cases) {
				println("{:48} {}", bench.name, bench.description);
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
		println(cerr, "sink={}", sink.load(memory_order_relaxed));
		return 0;
	} catch (exception const &ex) {
		println(cerr, "conflux_work_benchmarks: {}", ex.what());
		return 1;
	}
}
