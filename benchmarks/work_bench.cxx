#include <liburing.h>

import std;
import conflux.small_function;
import conflux.types;
import conflux.work;
import conflux.work.carrier.coro;

using namespace std::string_view_literals;

namespace ec = conflux::work::carrier;

namespace root = conflux::work::root;
struct OwnerCap {};
struct DriverCap {};
namespace conflux::work::root {

template<>
inline constexpr bool enable_address_capability_v<OwnerCap> = true;
template<>
inline constexpr bool enable_address_capability_v<DriverCap> = true;

} // namespace conflux::work::root
namespace {

inline std::atomic<std::size_t> sink{};
struct Config {
	bool list_only = false;
	std::string filter;
	std::optional<std::size_t> iterations_override;
	enum class Format : std::uint8_t {
		table,
		json,
	};
	Format format = Format::table;
};
struct Stats {
	std::string_view name;
	std::size_t iterations{};
	std::uint64_t total_ns{};
	double ns_per_iter{};
	double min_ns{};
	double p10_ns{};
	double mad_ns{};
};
using BenchFn = conflux::detail::small_move_only_function<std::size_t()>;
struct Case {
	std::string_view name;
	std::string_view description;
	std::size_t default_iterations;
	std::size_t reps = 10;
	BenchFn run;
};
void print_usage() {
	std::println("Usage: conflux_work_benchmarks [--list] [--filter SUBSTR] [--iterations N] [--format table|json]");
}
Config parse_args(
	std::span<char *> args) {
	Config cfg;
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view arg = args[i];
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
			std::size_t iters = 0;
			auto const value = std::string_view{args[++i]};
			auto const [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), iters);
			if (ec != std::errc{} || ptr != value.data() + value.size() || iters == 0) {
				throw std::invalid_argument{"--iterations must be a positive integer"};
			}
			cfg.iterations_override = iters;
			continue;
		}
		if (arg == "--samples" || arg == "--batch" || arg == "--warmup" || arg == "--config-name") {
			if (i + 1 >= args.size()) {
				throw std::invalid_argument{std::format("{} requires a value", arg)};
			}
			++i;
			continue;
		}
		if (arg == "--format") {
			if (i + 1 >= args.size()) {
				throw std::invalid_argument{"--format requires a value"};
			}
			auto const value = std::string_view{args[++i]};
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
		throw std::invalid_argument{std::format("unknown argument: {}", arg)};
	}
	return cfg;
}
bool matches_filter(
	Case const &bench,
	std::string_view filter) {
	return filter.empty() || bench.name.contains(filter) || bench.description.contains(filter);
}
std::size_t warmup_iterations(
	std::size_t iterations) {
	return std::clamp(iterations / 10, std::size_t{1}, std::size_t{1000});
}
Stats measure_case(
	Case const &bench,
	std::size_t iterations) {
	for (std::size_t i = 0; i < warmup_iterations(iterations); ++i) {
		sink.fetch_add(bench.run(), std::memory_order_relaxed);
	}

	std::vector<double> times;
	times.reserve(bench.reps);
	for (std::size_t r = 0; r < bench.reps; ++r) {
		auto const t0 = std::chrono::steady_clock::now();
		for (std::size_t i = 0; i < iterations; ++i) {
			sink.fetch_add(bench.run(), std::memory_order_relaxed);
		}
		auto const dt =
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
		times.push_back(static_cast<double>(dt) / static_cast<double>(iterations));
	}

	std::ranges::sort(times);
	std::size_t const n = times.size();

	auto const min_ns = times[0];
	auto const p10_ns = times[static_cast<std::size_t>(0.1 * static_cast<double>(n - 1))];

	double const med_ns = (n % 2 == 0) ? (times[n / 2 - 1] + times[n / 2]) / 2.0 : times[n / 2];

	std::vector<double> devs;
	devs.reserve(n);
	for (auto t: times) {
		devs.push_back(std::abs(t - med_ns));
	}
	std::ranges::sort(devs);
	double const mad_ns = (n % 2 == 0) ? (devs[n / 2 - 1] + devs[n / 2]) / 2.0 : devs[n / 2];

	auto const total_ns = static_cast<std::uint64_t>(med_ns * static_cast<double>(iterations));
	return Stats{
		.name = bench.name,
		.iterations = iterations,
		.total_ns = total_ns,
		.ns_per_iter = med_ns,
		.min_ns = min_ns,
		.p10_ns = p10_ns,
		.mad_ns = mad_ns,
	};
}
void print_header(
	Config::Format format) {
	if (format == Config::Format::table) {
		std::println(
			"{:48} {:>12} {:>10} {:>10} {:>10} {:>10}",
			"Benchmark",
			"Iterations",
			"med ns",
			"min ns",
			"p10 ns",
			"mad ns");
	}
}
void print_stats(
	Stats const &stats,
	Config::Format format) {
	if (format == Config::Format::table) {
		std::println(
			"{:48} {:>12} {:>10.1f} {:>10.1f} {:>10.1f} {:>10.1f}",
			stats.name,
			stats.iterations,
			stats.ns_per_iter,
			stats.min_ns,
			stats.p10_ns,
			stats.mad_ns);
	} else {
		std::println(
			"{{\"config\":\"\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},\"min\":{:."
			"2f},\"p10\":{:.2f},\"mad\":{:.2f}}}",
			stats.name,
			stats.iterations,
			stats.total_ns,
			stats.ns_per_iter,
			stats.min_ns,
			stats.p10_ns,
			stats.mad_ns);
	}
}
// ---------------------------------------------------------------------------
// root: lifecycle — value paths
// ---------------------------------------------------------------------------

Case make_root_task_value_case() {
	return Case{
		.name = "root/task_value",
		.description = "make_task_source + try_set_value + root::value(task)",
		.default_iterations = 200000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.try_set_value(root::Success<int>{42});
			return static_cast<std::size_t>(root::value(std::move(task)));
		}};
}
Case make_root_posted_value_case() {
	return Case{
		.name = "root/posted_value",
		.description = "make_posted_source + try_set_value + root::value(owner, posted)",
		.default_iterations = 200000,
		.run = [] {
			OwnerCap owner{};
			auto [posted, src] = root::make_posted_source<int>(owner);
			(void)src.try_set_value(root::Success<int>{42});
			return static_cast<std::size_t>(root::value(owner, std::move(posted)));
		}};
}
Case make_root_operation_value_case() {
	return Case{
		.name = "root/operation_value",
		.description = "make_operation_source + try_set_value + root::value(driver, op)",
		.default_iterations = 200000,
		.run = [] {
			DriverCap driver{};
			auto [op, src] = root::make_operation_source<int>(driver);
			(void)src.try_set_value(root::Success<int>{42});
			return static_cast<std::size_t>(root::value(driver, std::move(op)));
		}};
}
Case make_root_task_cancelled_case() {
	return Case{
		.name = "root/task_cancelled",
		.description = "make_task_source + try_set_cancelled + root::join (outcome check)",
		.default_iterations = 200000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.try_set_cancelled(root::work_errc::cancelled_requested);
			auto out = root::blocking_join(std::move(task));
			return static_cast<std::size_t>(out.is_cancelled() ? 1 : 0);
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
	return Case{.name = name, .description = desc, .default_iterations = 200000, .run = [enable_cancellation] {
					auto [task, src] =
						root::make_task_source<int>(root::SubmitOptions{.enable_cancellation = enable_cancellation});
					root::abandon_to(std::move(task), root::drop_on_abandon{});
					(void)src;
					return std::size_t{1};
				}};
}
Case make_root_abandon_drop_case() {
	return Case{
		.name = "root/abandon_drop",
		.description = "make_task_source + try_set_value + abandon_to(drop)",
		.default_iterations = 200000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			root::abandon_to(std::move(task), root::drop_on_abandon{});
			(void)src.try_set_value(root::Success<int>{1});
			return std::size_t{1};
		}};
}
Case make_root_abandon_sink_case() {
	struct Sink {
		std::size_t *seen{};
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
		.default_iterations = 200000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			std::size_t seen = 0;
			root::abandon_to(std::move(task), Sink{.seen = &seen});
			(void)src.try_set_cancelled(root::work_errc::cancelled_requested);
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
	return Case{.name = name, .description = desc, .default_iterations = 200000, .run = [enable_cancellation] {
					auto [task, src] =
						root::make_task_source<int>(root::SubmitOptions{.enable_cancellation = enable_cancellation});
					std::size_t seen = 0;
					(void)src.install_cancel_hook([&seen](root::CancelReason reason) noexcept {
						if (reason == root::CancelReason::requested) {
							++seen;
						}
					});
					auto control = task.control();
					std::size_t score = control.request_cancel() ? 1U : 0U;
					score += seen;
					root::abandon_to(std::move(task), root::drop_on_abandon{});
					return score;
				}};
}
// ---------------------------------------------------------------------------
// root: callable erasure — small_move_only_function
// ---------------------------------------------------------------------------

Case make_small_fn_inline_case() {
	using Fn = conflux::detail::small_move_only_function<void(root::CancelReason)>;
	return Case{
		.name = "root/small_fn_inline",
		.description = "small_move_only_function: construct + move + invoke (inline fit)",
		.default_iterations = 500000,
		.run = [] {
			std::size_t seen = 0;
			Fn fn{[&seen](root::CancelReason r) noexcept {
				if (r == root::CancelReason::requested) {
					++seen;
				}
			}};
			Fn moved{std::move(fn)};
			moved(root::CancelReason::requested);
			return seen;
		}};
}
Case make_small_fn_heap_case() {
	using Fn = conflux::detail::small_move_only_function<void(root::CancelReason)>;
	struct BigCapture {
		std::array<uintptr_t, 5> words{};
	};
	return Case{
		.name = "root/small_fn_heap",
		.description = "small_move_only_function: construct + move + invoke (heap alloc, >32B capture)",
		.default_iterations = 500000,
		.run = [] {
			std::size_t seen = 0;
			BigCapture cap{};
			cap.words[0] = 0xC0FFEEU;
			Fn fn{[&seen, cap](root::CancelReason r) noexcept {
				if (r == root::CancelReason::requested) {
					seen += (cap.words[0] & 1U) + 1U;
				}
			}};
			Fn moved{std::move(fn)};
			moved(root::CancelReason::requested);
			return seen;
		}};
}
// ---------------------------------------------------------------------------
// work: conflux::work::WorkPool dispatch
// ---------------------------------------------------------------------------

conflux::work::WorkPoolOptions bench_pool_opts() {
	conflux::work::WorkPoolOptions opts;
#ifdef CONFLUX_BENCH_SPIN_BEFORE_PARK
	opts.spin_before_park = CONFLUX_BENCH_SPIN_BEFORE_PARK;
#endif
	opts.threads = 4;
	return opts;
}
Case make_pool_single_case() {
	auto pool = std::make_shared<conflux::work::WorkPool>(bench_pool_opts());
	return Case{
		.name = "work/pool_single",
		.description = "conflux::work::async_run_on(pool, fn) + root::value(task) — single dispatch roundtrip",
		.default_iterations = 25000,
		.run = [pool] {
			return static_cast<std::size_t>(root::value(conflux::work::async_run_on(*pool, [] { return 42; })));
		}};
}
Case make_pool_join_all_3_case() {
	auto pool = std::make_shared<conflux::work::WorkPool>(bench_pool_opts());
	return Case{
		.name = "work/pool_join_all_3",
		.description = "conflux::work::join_all(3 × run_on_task) + root::value — 3-way fan-out",
		.default_iterations = 30000,
		.run = [pool] {
			auto [a, b, c] = root::value(
				conflux::work::join_all(
					conflux::work::async_run_on(*pool, [] { return 1; }),
					conflux::work::async_run_on(*pool, [] { return 2; }),
					conflux::work::async_run_on(*pool, [] { return 3; })));
			return static_cast<std::size_t>(a + b + c);
		}};
}
Case make_pool_bursty_case() {
	auto pool = std::make_shared<conflux::work::WorkPool>(bench_pool_opts());
	return Case{
		.name = "work/pool_bursty_8",
		.description = "burst of 8 tasks after idle gap — exercises park/wake path",
		.default_iterations = 10000,
		.run = [pool] {
			// Historical benchmark behavior: short spin gap, not OS sleep.
			for (int i = 0; i < 200; ++i) {
				std::atomic_signal_fence(std::memory_order_seq_cst);
			}
			// Burst 8 tasks
			auto t0 = conflux::work::async_run_on(*pool, [] { return 1; });
			auto t1 = conflux::work::async_run_on(*pool, [] { return 2; });
			auto t2 = conflux::work::async_run_on(*pool, [] { return 3; });
			auto t3 = conflux::work::async_run_on(*pool, [] { return 4; });
			auto t4 = conflux::work::async_run_on(*pool, [] { return 5; });
			auto t5 = conflux::work::async_run_on(*pool, [] { return 6; });
			auto t6 = conflux::work::async_run_on(*pool, [] { return 7; });
			auto t7 = conflux::work::async_run_on(*pool, [] { return 8; });
			auto [a, b, c, d, e, f, g, h] = root::value(
				conflux::work::join_all(
					std::move(t0),
					std::move(t1),
					std::move(t2),
					std::move(t3),
					std::move(t4),
					std::move(t5),
					std::move(t6),
					std::move(t7)));
			return static_cast<std::size_t>(a + b + c + d + e + f + g + h);
		}};
}
// ---------------------------------------------------------------------------
// work: EagerChain coroutine microbench
// ---------------------------------------------------------------------------

ec::EagerChain<int> ec_l1() {
	co_return 1;
}
ec::EagerChain<int> ec_l2() {
	co_return 1 + co_await ec_l1();
}
ec::EagerChain<int> ec_l3() {
	co_return 1 + co_await ec_l2();
}
ec::EagerChain<int> ec_l4() {
	co_return 1 + co_await ec_l3();
}
Case make_eager_chain_flat_int_case() {
	return Case{
		.name = "eager_chain/flat_int",
		.description = "EagerChain<int>: allocate frame + co_return int (single frame)",
		.default_iterations = 2000000,
		.run = [] {
			auto c = []() -> ec::EagerChain<int> { co_return 42; }();
			auto out = std::move(c).chain().release_outcome();
			return static_cast<std::size_t>(std::move(out).success().value);
		}};
}
Case make_eager_chain_flat_void_case() {
	return Case{
		.name = "eager_chain/flat_void",
		.description = "EagerChain<void>: allocate frame + co_return void (single frame)",
		.default_iterations = 2000000,
		.run = [] {
			auto c = []() -> ec::EagerChain<void> { co_return; }();
			(void)std::move(c).chain().release_outcome();
			return std::size_t{1};
		}};
}
Case make_eager_chain_nested_4_case() {
	return Case{
		.name = "eager_chain/nested_4",
		.description = "EagerChain<int> 4-deep: LIFO frame stack, all synchronous",
		.default_iterations = 500000,
		.run = [] {
			auto out = ec_l4().chain().release_outcome();
			return static_cast<std::size_t>(std::move(out).success().value);
		}};
}
// ---------------------------------------------------------------------------
// work: conflux::work::RingLane
// ---------------------------------------------------------------------------

Case make_ring_lane_case() {
	struct State {
		io_uring ring{};
		std::unique_ptr<conflux::work::RingLane> lane;
		State() {
			int const rc = ::io_uring_queue_init(8, &ring, 0);
			if (rc != 0) {
				throw std::runtime_error{std::format("io_uring_queue_init failed: {}", rc)};
			}
			lane = std::make_unique<conflux::work::RingLane>(conflux::work::RingLaneOptions{
				.ring_fd = ring.ring_fd,
				.wake_user_data = 0x57524B42U,
				.drain_budget = 0,
				.allow_inline_on_owner = false,
			});
			lane->adopt_current_thread();
		}
		~State() { ::io_uring_queue_exit(&ring); }
	};
	auto state = std::make_shared<State>();
	return Case{
		.name = "work/ring_lane_roundtrip",
		.description = "conflux::work::RingLane enqueue from std::jthread + msg-ring wake + owner drain",
		.default_iterations = 5000,
		.reps = 1,
		.run = [state] {
			std::atomic<std::size_t> out{};
			std::jthread producer([&] {
				if (!state->lane->enqueue([&out] { out.store(77, std::memory_order_release); })) {
					throw std::runtime_error{"ring lane enqueue failed"};
				}
			});
			producer.join();
			io_uring_cqe *cqe = nullptr;
			if (::io_uring_wait_cqe(&state->ring, &cqe) != 0 || cqe == nullptr) {
				throw std::runtime_error{"io_uring_wait_cqe failed"};
			}
			::io_uring_cqe_seen(&state->ring, cqe);
			(void)state->lane->drain();
			return out.load(std::memory_order_acquire);
		}};
}
std::vector<Case> make_cases() {
	std::vector<Case> cases;
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
	cases.push_back(make_pool_bursty_case());
	// work: ring lane
	try {
		cases.push_back(make_ring_lane_case());
	} catch (std::exception const &) {}
	// work: EagerChain microbench
	cases.push_back(make_eager_chain_flat_int_case());
	cases.push_back(make_eager_chain_flat_void_case());
	cases.push_back(make_eager_chain_nested_4_case());
	return cases;
}

} // namespace
int main(
	int argc,
	char **argv) {
	if (argc >= 2 && std::string_view{argv[1]} == "--bench-info") {
		std::print(
			"{}\n",
			R"({"name":"work","parser":"standard","configs":[{"name":"default","extra":{},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--iterations","0"]}]})");
		return 0;
	}
	try {
		auto const cfg = parse_args({argv, static_cast<std::size_t>(argc)});
		auto cases = make_cases();
		if (cfg.list_only) {
			for (auto const &bench: cases) {
				std::println("{:48} {}", bench.name, bench.description);
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
		std::println(std::cerr, "sink={}", sink.load(std::memory_order_relaxed));
		return 0;
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_work_benchmarks: {}", ex.what());
		return 1;
	}
}
