#include <liburing.h>

import std;
import conflux.types;
import conflux.work;
#if CONFLUX_WORK_CARRIER_MODEL_A
import conflux.work.carrier.model_a;
#endif
#if CONFLUX_WORK_CARRIER_MODEL_B
import conflux.work.carrier.model_b;
#endif
import conflux.work.carrier.deadline;

using namespace std::string_view_literals;

namespace root = conflux::work::root;
#if CONFLUX_WORK_CARRIER_MODEL_A
namespace model_a = conflux::work::carrier::model_a;
#endif
#if CONFLUX_WORK_CARRIER_MODEL_B
namespace model_b = conflux::work::carrier::model_b;
#endif
namespace carrier = conflux::work::carrier;

namespace {

inline Atom<SZ> sink{};

struct OwnerCap : root::capability_id_from_address<OwnerCap> {};

struct DriverCap : root::capability_id_from_address<DriverCap> {};

struct Config {
	bool list_only = false;
	S filter;
	Opt<SZ> iterations_override;
	enum class Format : u8 {
		table,
		csv,
	};
	Format format = Format::table;
};

struct Stats {
	SV name;
	SZ iterations{};
	u64 total_ns{};
	double ns_per_iter{};
};

using BenchFn = root::detail::MoveOnlyFunction<SZ()>;

struct Case {
	SV name;
	SV description;
	SZ default_iterations;
	BenchFn run;
};

void print_usage() {
	println("Usage: conflux_work_benchmarks [--list] [--filter SUBSTR] [--iterations N] [--format table|csv]");
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
			} else if (value == "csv") {
				cfg.format = Config::Format::csv;
			} else {
				throw std::invalid_argument{"--format must be table or csv"};
			}
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
		.run = [] { return static_cast<SZ>(wait(value(7) | then([](int x) { return x + 1; }))); }};
}

Case make_pool_roundtrip_case() {
	auto pool = make_shared<WorkPool>();
	return Case{
		.name = "micro/pool_roundtrip",
		.description = "Submit to WorkPool and wait for one result",
		.default_iterations = 100'000,
		.run = [pool] { return static_cast<SZ>(wait(run_on(*pool, [] { return 42; }))); }};
}

Case make_pool_chain_case() {
	auto pool = make_shared<WorkPool>();
	return Case{
		.name = "micro/pool_chain",
		.description = "WorkPool submit followed by chained continuation",
		.default_iterations = 100'000,
		.run = [pool] {
			return static_cast<SZ>(
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
			return static_cast<SZ>(a + b + c);
		}};
}

Case make_ring_lane_case() {
	struct State {
		io_uring ring{};
		UP<RingLane> lane;

		State()
			: lane{} {
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
		.name = "micro/ring_lane_roundtrip",
		.description = "Foreign-thread enqueue, msg-ring wake, owner-thread drain",
		.default_iterations = 20'000,
		.run = [state] {
			Atom<SZ> value_out{};
			jthread producer([&] {
				bool const queued = state->lane->enqueue([&] { value_out.store(77, memory_order_release); });
				if (!queued) {
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
			return value_out.load(memory_order_acquire);
		}};
}

Case make_root_task_join_case() {
	return Case{
		.name = "root/task_join_success",
		.description = "make_task_source + commit_success + value(join)",
		.default_iterations = 200'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			bool const committed = src.commit_success(root::Success<int>{9});
			if (!committed) {
				throw RE{"commit_success failed"};
			}
			return static_cast<SZ>(root::value(move(task)));
		}};
}

Case make_root_posted_join_case() {
	return Case{
		.name = "root/posted_join_success",
		.description = "make_posted_source + commit_success + value(join)",
		.default_iterations = 200'000,
		.run = [] {
			OwnerCap owner{};
			auto [posted, src] = root::make_posted_source<int>(owner);
			bool const committed = src.commit_success(root::Success<int>{7});
			if (!committed) {
				throw RE{"commit_success failed"};
			}
			return static_cast<SZ>(root::value(owner, move(posted)));
		}};
}

Case make_root_operation_join_case() {
	return Case{
		.name = "root/operation_join_success",
		.description = "make_operation_source + commit_success + value(join)",
		.default_iterations = 200'000,
		.run = [] {
			DriverCap driver{};
			auto [op, src] = root::make_operation_source<int>(driver);
			bool const committed = src.commit_success(root::Success<int>{5});
			if (!committed) {
				throw RE{"commit_success failed"};
			}
			return static_cast<SZ>(root::value(driver, move(op)));
		}};
}

Case make_root_task_admission_case(
	bool enable_cancellation) {
	auto const name = enable_cancellation ? "root/task_admission_enabled"sv : "root/task_admission_disabled"sv;
	auto const description = enable_cancellation ? "make_task_source admission with cancellation enabled" :
												   "make_task_source admission with cancellation disabled";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					auto [task, src] =
						root::make_task_source<int>(root::SubmitOptions{.enable_cancellation = enable_cancellation});
					root::abandon_to(move(task), root::drop_on_abandon{});
					(void)src;
					return SZ{1};
				}};
}

Case make_root_posted_admission_case(
	bool enable_cancellation) {
	auto const name = enable_cancellation ? "root/posted_admission_enabled"sv : "root/posted_admission_disabled"sv;
	auto const description = enable_cancellation ? "make_posted_source admission with cancellation enabled" :
												   "make_posted_source admission with cancellation disabled";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					OwnerCap owner{};
					auto [posted, src] = root::make_posted_source<int>(
						owner,
						root::PostOptions{.enable_cancellation = enable_cancellation});
					root::abandon_to(move(posted), root::drop_on_abandon{});
					(void)src;
					return SZ{1};
				}};
}

Case make_root_operation_admission_case(
	bool enable_cancellation) {
	auto const name =
		enable_cancellation ? "root/operation_admission_enabled"sv : "root/operation_admission_disabled"sv;
	auto const description = enable_cancellation ? "make_operation_source admission with cancellation enabled" :
												   "make_operation_source admission with cancellation disabled";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					DriverCap driver{};
					auto [op, src] = root::make_operation_source<int>(
						driver,
						root::OperationOptions{.enable_cancellation = enable_cancellation});
					root::abandon_to(move(op), root::drop_on_abandon{});
					(void)src;
					return SZ{1};
				}};
}

Case make_root_task_control_admission_case(
	bool enable_cancellation) {
	auto const name =
		enable_cancellation ? "root/task_control_admission_enabled"sv : "root/task_control_admission_disabled"sv;
	auto const description = enable_cancellation ? "make_task_control_source admission with cancellation enabled" :
												   "make_task_control_source admission with cancellation disabled";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					auto [control, src] = root::make_task_control_source<int>(
						root::SubmitOptions{.enable_cancellation = enable_cancellation});
					(void)control;
					(void)src;
					return SZ{1};
				}};
}

Case make_root_posted_control_admission_case(
	bool enable_cancellation) {
	auto const name =
		enable_cancellation ? "root/posted_control_admission_enabled"sv : "root/posted_control_admission_disabled"sv;
	auto const description = enable_cancellation ? "make_posted_control_source admission with cancellation enabled" :
												   "make_posted_control_source admission with cancellation disabled";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					auto [control, src] = root::make_posted_control_source<int>(
						root::PostOptions{.enable_cancellation = enable_cancellation});
					(void)control;
					(void)src;
					return SZ{1};
				}};
}

Case make_root_operation_control_admission_case(
	bool enable_cancellation) {
	auto const name = enable_cancellation ? "root/operation_control_admission_enabled"sv :
											"root/operation_control_admission_disabled"sv;
	auto const description = enable_cancellation ? "make_operation_control_source admission with cancellation enabled" :
												   "make_operation_control_source admission with cancellation disabled";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					auto [control, src] = root::make_operation_control_source<int>(
						root::OperationOptions{.enable_cancellation = enable_cancellation});
					(void)control;
					(void)src;
					return SZ{1};
				}};
}

Case make_root_cancel_hook_case(
	bool enable_cancellation) {
	auto const name = enable_cancellation ? "root/cancel_hook_enabled"sv : "root/cancel_hook_disabled"sv;
	auto const description = enable_cancellation ? "install_cancel_hook + request_cancel with stop-token enabled"sv :
												   "install_cancel_hook + request_cancel with inert stop-token path"sv;
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					auto [task, src] =
						root::make_task_source<int>(root::SubmitOptions{.enable_cancellation = enable_cancellation});
					SZ seen = 0;
					bool const installed = src.install_cancel_hook([&seen](root::CancelReason reason) noexcept {
						if (reason == root::CancelReason::requested) {
							++seen;
						}
					});
					if (!installed) {
						throw RE{"install_cancel_hook failed"};
					}
					auto control = task.control();
					SZ score = control.request_cancel() ? 1U : 0U;
					score += seen;
					root::abandon_to(move(task), root::drop_on_abandon{});
					return score;
				}};
}

Case make_root_posted_cancel_hook_case(
	bool enable_cancellation) {
	auto const name = enable_cancellation ? "root/posted_cancel_hook_enabled"sv : "root/posted_cancel_hook_disabled"sv;
	auto const description = enable_cancellation ? "posted install_cancel_hook + request_cancel (enabled)" :
												   "posted install_cancel_hook + request_cancel (disabled)";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					OwnerCap owner{};
					auto [posted, src] = root::make_posted_source<int>(
						owner,
						root::PostOptions{.enable_cancellation = enable_cancellation});
					SZ seen = 0;
					bool const installed = src.install_cancel_hook([&seen](root::CancelReason reason) noexcept {
						if (reason == root::CancelReason::requested) {
							++seen;
						}
					});
					if (!installed) {
						throw RE{"install_cancel_hook failed"};
					}
					auto control = posted.control();
					SZ score = control.request_cancel() ? 1U : 0U;
					score += seen;
					root::abandon_to(move(posted), root::drop_on_abandon{});
					return score;
				}};
}

Case make_root_operation_cancel_hook_case(
	bool enable_cancellation) {
	auto const name =
		enable_cancellation ? "root/operation_cancel_hook_enabled"sv : "root/operation_cancel_hook_disabled"sv;
	auto const description = enable_cancellation ? "operation install_cancel_hook + request_cancel (enabled)" :
												   "operation install_cancel_hook + request_cancel (disabled)";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					DriverCap driver{};
					auto [op, src] = root::make_operation_source<int>(
						driver,
						root::OperationOptions{.enable_cancellation = enable_cancellation});
					SZ seen = 0;
					bool const installed = src.install_cancel_hook([&seen](root::CancelReason reason) noexcept {
						if (reason == root::CancelReason::requested) {
							++seen;
						}
					});
					if (!installed) {
						throw RE{"install_cancel_hook failed"};
					}
					auto control = op.control();
					SZ score = control.request_cancel() ? 1U : 0U;
					score += seen;
					root::abandon_to(move(op), root::drop_on_abandon{});
					return score;
				}};
}

Case make_root_control_cancel_hook_case(
	bool enable_cancellation) {
	auto const name =
		enable_cancellation ? "root/control_cancel_hook_enabled"sv : "root/control_cancel_hook_disabled"sv;
	auto const description = enable_cancellation ?
								 "make_task_control_source + install_cancel_hook + request_cancel (enabled)" :
								 "make_task_control_source + install_cancel_hook + request_cancel (disabled)";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					auto [control, src] = root::make_task_control_source<int>(
						root::SubmitOptions{.enable_cancellation = enable_cancellation});
					SZ seen = 0;
					bool const installed = src.install_cancel_hook([&seen](root::CancelReason reason) noexcept {
						if (reason == root::CancelReason::requested) {
							++seen;
						}
					});
					if (!installed) {
						throw RE{"install_cancel_hook failed"};
					}
					SZ score = control.request_cancel() ? 1U : 0U;
					score += seen;
					return score;
				}};
}

Case make_root_posted_control_cancel_hook_case(
	bool enable_cancellation) {
	auto const name = enable_cancellation ? "root/posted_control_cancel_hook_enabled"sv :
											"root/posted_control_cancel_hook_disabled"sv;
	auto const description = enable_cancellation ?
								 "make_posted_control_source + install_cancel_hook + request_cancel (enabled)" :
								 "make_posted_control_source + install_cancel_hook + request_cancel (disabled)";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					auto [control, src] = root::make_posted_control_source<int>(
						root::PostOptions{.enable_cancellation = enable_cancellation});
					SZ seen = 0;
					bool const installed = src.install_cancel_hook([&seen](root::CancelReason reason) noexcept {
						if (reason == root::CancelReason::requested) {
							++seen;
						}
					});
					if (!installed) {
						throw RE{"install_cancel_hook failed"};
					}
					SZ score = control.request_cancel() ? 1U : 0U;
					score += seen;
					return score;
				}};
}

Case make_root_operation_control_cancel_hook_case(
	bool enable_cancellation) {
	auto const name = enable_cancellation ? "root/operation_control_cancel_hook_enabled"sv :
											"root/operation_control_cancel_hook_disabled"sv;
	auto const description = enable_cancellation ?
								 "make_operation_control_source + install_cancel_hook + request_cancel (enabled)" :
								 "make_operation_control_source + install_cancel_hook + request_cancel (disabled)";
	return Case{.name = name, .description = description, .default_iterations = 200'000, .run = [enable_cancellation] {
					auto [control, src] = root::make_operation_control_source<int>(
						root::OperationOptions{.enable_cancellation = enable_cancellation});
					SZ seen = 0;
					bool const installed = src.install_cancel_hook([&seen](root::CancelReason reason) noexcept {
						if (reason == root::CancelReason::requested) {
							++seen;
						}
					});
					if (!installed) {
						throw RE{"install_cancel_hook failed"};
					}
					SZ score = control.request_cancel() ? 1U : 0U;
					score += seen;
					return score;
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
		.name = "root/abandon_sink_cancelled",
		.description = "abandon_to sink dispatch on cancelled terminal outcome",
		.default_iterations = 200'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			SZ seen = 0;
			root::abandon_to(move(task), Sink{.seen = &seen});
			bool const committed = src.commit_cancelled(root::CancelReason::requested);
			if (!committed) {
				throw RE{"commit_cancelled failed"};
			}
			return seen;
		}};
}

template<typename Fn>
Case make_callable_erasure_case(
	SV name,
	SV description) {
	return Case{.name = name, .description = description, .default_iterations = 500'000, .run = [] {
					SZ seen = 0;
					Fn fn{[&seen](root::CancelReason reason) noexcept {
						if (reason == root::CancelReason::requested) {
							++seen;
						}
					}};
					Fn moved{move(fn)};
					moved(root::CancelReason::requested);
					return seen;
				}};
}

template<typename Fn, SZ CaptureWords>
Case make_callable_erasure_capture_case(
	SV name,
	SV description) {
	struct Payload {
		A<uintptr_t, CaptureWords> words{};
	};

	return Case{.name = name, .description = description, .default_iterations = 500'000, .run = [] {
					SZ seen = 0;
					Payload payload{};
					payload.words[0] = 0xC0FFEEU;
					Fn fn{[&seen, payload](root::CancelReason reason) noexcept {
						if (reason == root::CancelReason::requested) {
							seen += static_cast<SZ>(payload.words[0] & 1U) + 1U;
						}
					}};
					Fn moved{move(fn)};
					moved(root::CancelReason::requested);
					return seen;
				}};
}

#if CONFLUX_WORK_CARRIER_MODEL_A

struct OwnerCapA : root::capability_id_from_address<OwnerCapA> {};
struct DriverCapA : root::capability_id_from_address<DriverCapA> {};

Case make_carrier_a_task_map1_case() {
	return Case{
		.name = "carrier_a/task_map1",
		.description = "Model A: from_task + 1 map + extract",
		.default_iterations = 500'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.commit_success(root::Success<int>{42});
			auto chain = model_a::from_task(move(task));
			auto mapped = model_a::map(move(chain), [](int x) { return x + 1; });
			auto out = move(mapped).release_outcome();
			return static_cast<SZ>(out.is_success() ? out.success().value : 0);
		}};
}

Case make_carrier_a_task_map3_case() {
	return Case{
		.name = "carrier_a/task_map3",
		.description = "Model A: from_task + 3 maps + extract",
		.default_iterations = 500'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.commit_success(root::Success<int>{1});
			auto c0 = model_a::from_task(move(task));
			auto c1 = model_a::map(move(c0), [](int x) { return x + 1; });
			auto c2 = model_a::map(move(c1), [](int x) { return x * 2; });
			auto c3 = model_a::map(move(c2), [](int x) { return x - 1; });
			auto out = move(c3).release_outcome();
			return static_cast<SZ>(out.is_success() ? out.success().value : 0);
		}};
}

Case make_carrier_a_cancel_passthru_case() {
	return Case{
		.name = "carrier_a/cancel_passthru",
		.description = "Model A: from_task(cancelled) + map passthrough",
		.default_iterations = 500'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.commit_cancelled(root::CancelReason::requested);
			auto chain = model_a::from_task(move(task));
			auto mapped = model_a::map(move(chain), [](int x) { return x + 1; });
			auto out = move(mapped).release_outcome();
			return static_cast<SZ>(out.is_cancelled() ? 1 : 0);
		}};
}

Case make_carrier_a_mixed_3stage_case() {
	return Case{
		.name = "carrier_a/mixed_3stage",
		.description = "Model A: from_task + hop_to_posted + hop_to_operation + extract",
		.default_iterations = 500'000,
		.run = [] {
			OwnerCapA owner{};
			DriverCapA driver{};
			auto [task, src] = root::make_task_source<int>();
			(void)src.commit_success(root::Success<int>{7});
			auto c0 = model_a::from_task(move(task));
			auto c1 = model_a::hop_to_posted(owner, move(c0));
			auto c2 = model_a::hop_to_operation(driver, move(c1));
			auto out = move(c2).release_outcome();
			return static_cast<SZ>(out.is_success() ? out.success().value : 0);
		}};
}

Case make_carrier_a_hop_verify_case() {
	return Case{
		.name = "carrier_a/hop_verify",
		.description = "Model A: from_task + hop_to_posted + verify_hop (matching) + extract",
		.default_iterations = 500'000,
		.run = [] {
			OwnerCapA owner{};
			auto [task, src] = root::make_task_source<int>();
			(void)src.commit_success(root::Success<int>{7});
			auto c0 = model_a::from_task(move(task));
			auto c1 = model_a::hop_to_posted(owner, move(c0));
			model_a::verify_hop(owner, c1);
			auto out = move(c1).release_outcome();
			return static_cast<SZ>(out.is_success() ? out.success().value : 0);
		}};
}

Case make_carrier_a_when_all_case() {
	return Case{
		.name = "carrier_a/when_all",
		.description = "Model A: when_all(2 task chains) + extract",
		.default_iterations = 200'000,
		.run = [] {
			auto [ta, sa] = root::make_task_source<int>();
			auto [tb, sb] = root::make_task_source<int>();
			(void)sa.commit_success(root::Success<int>{10});
			(void)sb.commit_success(root::Success<int>{20});
			auto ca = model_a::from_task(move(ta));
			auto cb = model_a::from_task(move(tb));
			auto combined = model_a::when_all(move(ca), move(cb));
			auto out = move(combined).release_outcome();
			if (!out.is_success()) {
				throw RE{"when_all failed"};
			}
			auto [a, b] = move(out).success().value;
			return static_cast<SZ>(a + b);
		}};
}

Case make_carrier_a_when_all_fast_fail_case() {
	return Case{
		.name = "carrier_a/when_all_fast_fail",
		.description = "Model A: when_all_fast_fail(2 task chains) + extract",
		.default_iterations = 200'000,
		.run = [] {
			auto [ta, sa] = root::make_task_source<int>();
			auto [tb, sb] = root::make_task_source<int>();
			(void)sa.commit_success(root::Success<int>{10});
			(void)sb.commit_success(root::Success<int>{20});
			auto ca = model_a::from_task(move(ta));
			auto cb = model_a::from_task(move(tb));
			auto combined = model_a::when_all_fast_fail(move(ca), move(cb));
			auto out = move(combined).release_outcome();
			if (!out.is_success()) {
				throw RE{"when_all_fast_fail failed"};
			}
			auto [a, b] = move(out).success().value;
			return static_cast<SZ>(a + b);
		}};
}

Case make_carrier_a_race_a_wins_case() {
	return Case{
		.name = "carrier_a/race_a_wins",
		.description = "Model A: race(success, success) — a wins",
		.default_iterations = 200'000,
		.run = [] {
			auto [ta, sa] = root::make_task_source<int>();
			auto [tb, sb] = root::make_task_source<int>();
			(void)sa.commit_success(root::Success<int>{7});
			(void)sb.commit_success(root::Success<int>{99});
			auto ca = model_a::from_task(move(ta));
			auto cb = model_a::from_task(move(tb));
			auto winner = model_a::race(move(ca), move(cb));
			auto out = move(winner).release_outcome();
			if (!out.is_success()) {
				throw RE{"race failed"};
			}
			return static_cast<SZ>(out.success().value);
		}};
}

Case make_carrier_a_race_b_wins_case() {
	return Case{
		.name = "carrier_a/race_b_wins",
		.description = "Model A: race(failure, success) — b wins",
		.default_iterations = 200'000,
		.run = [] {
			auto [ta, sa] = root::make_task_source<int>();
			auto [tb, sb] = root::make_task_source<int>();
			(void)sa.commit_failure(make_exception_ptr(RE{"fail"}));
			(void)sb.commit_success(root::Success<int>{5});
			auto ca = model_a::from_task(move(ta));
			auto cb = model_a::from_task(move(tb));
			auto winner = model_a::race(move(ca), move(cb));
			auto out = move(winner).release_outcome();
			if (!out.is_success()) {
				throw RE{"race b-wins failed"};
			}
			return static_cast<SZ>(out.success().value);
		}};
}

#endif // CONFLUX_WORK_CARRIER_MODEL_A

Case make_deadline_scope_arm_disarm_case() {
	return Case{
		.name = "carrier/deadline_scope_arm_disarm",
		.description = "DeadlineScope(60s) arm + immediate destroy (jthread start+stop)",
		.default_iterations = 5'000,
		.run = [] {
			carrier::DeadlineScope const scope{chrono::seconds{60}};
			return SZ{1};
		}};
}

#if CONFLUX_WORK_CARRIER_MODEL_A

Case make_deadline_scope_fast_path_case() {
	return Case{
		.name = "carrier/deadline_scope_admit_fast",
		.description = "DeadlineScope(60s) + admit(pre-resolved task) — deadline never fires",
		.default_iterations = 5'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.commit_success(root::Success<int>{42});
			auto jh = root::into_join_handle(move(task));
			carrier::DeadlineScope scope{chrono::seconds{60}};
			auto chain = scope.admit(move(jh));
			auto out = move(chain).release_outcome();
			if (!out.is_success()) {
				throw RE{"deadline fast path failed"};
			}
			return static_cast<SZ>(out.success().value);
		}};
}

#endif // CONFLUX_WORK_CARRIER_MODEL_A (deadline fast path)

#if CONFLUX_WORK_CARRIER_MODEL_B

struct OwnerCapB : root::capability_id_from_address<OwnerCapB> {};
struct DriverCapB : root::capability_id_from_address<DriverCapB> {};

Case make_carrier_b_task_map1_case() {
	return Case{
		.name = "carrier_b/task_map1",
		.description = "Model B: from_task + 1 map + extract",
		.default_iterations = 500'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.commit_success(root::Success<int>{42});
			auto chain = model_b::from_task(move(task));
			auto mapped = model_b::map(move(chain), [](int x) { return x + 1; });
			auto out = move(mapped).release_outcome();
			return static_cast<SZ>(out.is_success() ? out.success().value : 0);
		}};
}

Case make_carrier_b_task_map3_case() {
	return Case{
		.name = "carrier_b/task_map3",
		.description = "Model B: from_task + 3 maps + extract",
		.default_iterations = 500'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.commit_success(root::Success<int>{1});
			auto c0 = model_b::from_task(move(task));
			auto c1 = model_b::map(move(c0), [](int x) { return x + 1; });
			auto c2 = model_b::map(move(c1), [](int x) { return x * 2; });
			auto c3 = model_b::map(move(c2), [](int x) { return x - 1; });
			auto out = move(c3).release_outcome();
			return static_cast<SZ>(out.is_success() ? out.success().value : 0);
		}};
}

Case make_carrier_b_cancel_passthru_case() {
	return Case{
		.name = "carrier_b/cancel_passthru",
		.description = "Model B: from_task(cancelled) + map passthrough",
		.default_iterations = 500'000,
		.run = [] {
			auto [task, src] = root::make_task_source<int>();
			(void)src.commit_cancelled(root::CancelReason::requested);
			auto chain = model_b::from_task(move(task));
			auto mapped = model_b::map(move(chain), [](int x) { return x + 1; });
			auto out = move(mapped).release_outcome();
			return static_cast<SZ>(out.is_cancelled() ? 1 : 0);
		}};
}

Case make_carrier_b_mixed_3stage_case() {
	return Case{
		.name = "carrier_b/mixed_3stage",
		.description = "Model B: from_task + hop_to_posted + hop_to_operation + extract",
		.default_iterations = 500'000,
		.run = [] {
			OwnerCapB owner{};
			DriverCapB driver{};
			auto [task, src] = root::make_task_source<int>();
			(void)src.commit_success(root::Success<int>{7});
			auto c0 = model_b::from_task(move(task));
			auto c1 = model_b::hop_to_posted(owner, move(c0));
			auto c2 = model_b::hop_to_operation(driver, move(c1));
			auto out = move(c2).release_outcome();
			return static_cast<SZ>(out.is_success() ? out.success().value : 0);
		}};
}

Case make_carrier_b_when_all_case() {
	return Case{
		.name = "carrier_b/when_all",
		.description = "Model B: when_all(2 task chains) + extract",
		.default_iterations = 200'000,
		.run = [] {
			auto [ta, sa] = root::make_task_source<int>();
			auto [tb, sb] = root::make_task_source<int>();
			(void)sa.commit_success(root::Success<int>{10});
			(void)sb.commit_success(root::Success<int>{20});
			auto ca = model_b::from_task(move(ta));
			auto cb = model_b::from_task(move(tb));
			auto combined = model_b::when_all(move(ca), move(cb));
			auto out = move(combined).release_outcome();
			if (!out.is_success()) {
				throw RE{"when_all failed"};
			}
			auto [a, b] = move(out).success().value;
			return static_cast<SZ>(a + b);
		}};
}

#endif // CONFLUX_WORK_CARRIER_MODEL_B

V<Case> make_cases() {
	V<Case> cases;
	cases.push_back(make_value_then_case());
	cases.push_back(make_pool_roundtrip_case());
	cases.push_back(make_pool_chain_case());
	cases.push_back(make_join_all_case());
	cases.push_back(make_root_task_join_case());
	cases.push_back(make_root_posted_join_case());
	cases.push_back(make_root_operation_join_case());
	cases.push_back(make_root_task_admission_case(true));
	cases.push_back(make_root_task_admission_case(false));
	cases.push_back(make_root_posted_admission_case(true));
	cases.push_back(make_root_posted_admission_case(false));
	cases.push_back(make_root_operation_admission_case(true));
	cases.push_back(make_root_operation_admission_case(false));
	cases.push_back(make_root_task_control_admission_case(true));
	cases.push_back(make_root_task_control_admission_case(false));
	cases.push_back(make_root_posted_control_admission_case(true));
	cases.push_back(make_root_posted_control_admission_case(false));
	cases.push_back(make_root_operation_control_admission_case(true));
	cases.push_back(make_root_operation_control_admission_case(false));
	cases.push_back(make_root_cancel_hook_case(true));
	cases.push_back(make_root_cancel_hook_case(false));
	cases.push_back(make_root_posted_cancel_hook_case(true));
	cases.push_back(make_root_posted_cancel_hook_case(false));
	cases.push_back(make_root_operation_cancel_hook_case(true));
	cases.push_back(make_root_operation_cancel_hook_case(false));
	cases.push_back(make_root_control_cancel_hook_case(true));
	cases.push_back(make_root_control_cancel_hook_case(false));
	cases.push_back(make_root_posted_control_cancel_hook_case(true));
	cases.push_back(make_root_posted_control_cancel_hook_case(false));
	cases.push_back(make_root_operation_control_cancel_hook_case(true));
	cases.push_back(make_root_operation_control_cancel_hook_case(false));
	cases.push_back(make_root_abandon_sink_case());
	cases.push_back(
		make_callable_erasure_case<root::detail::MoveOnlyFunction<void(root::CancelReason)>>(
			"root/callable_erasure_custom",
			"construct + move + invoke root::detail::MoveOnlyFunction"));
	cases.push_back(
		make_callable_erasure_case<root::detail::MoveOnlyFunction<void(root::CancelReason), 24>>(
			"root/callable_erasure_custom_inline24",
			"construct + move + invoke root::detail::MoveOnlyFunction<...,24>"));
	cases.push_back(
		make_callable_erasure_case<root::detail::MoveOnlyFunction<void(root::CancelReason), 32>>(
			"root/callable_erasure_custom_inline32",
			"construct + move + invoke root::detail::MoveOnlyFunction<...,32>"));
	cases.push_back(
		make_callable_erasure_case<Fn<void(root::CancelReason)>>(
			"root/callable_erasure_std_function",
			"construct + move + invoke Fn baseline"));
	cases.push_back(
		make_callable_erasure_capture_case<root::detail::MoveOnlyFunction<void(root::CancelReason)>, 3>(
			"root/callable_erasure_custom_capture24",
			"capture24 + construct + move + invoke root::detail::MoveOnlyFunction"));
	cases.push_back(
		make_callable_erasure_capture_case<Fn<void(root::CancelReason)>, 3>(
			"root/callable_erasure_std_function_capture24",
			"capture24 + construct + move + invoke Fn baseline"));
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
	cases.push_back(
		make_callable_erasure_case<std::move_only_function<void(root::CancelReason)>>(
			"root/callable_erasure_std",
			"construct + move + invoke std::move_only_function"));
#endif
	try {
		cases.push_back(make_ring_lane_case());
	} catch (exception const &) {}
#if CONFLUX_WORK_CARRIER_MODEL_A
	cases.push_back(make_carrier_a_task_map1_case());
	cases.push_back(make_carrier_a_task_map3_case());
	cases.push_back(make_carrier_a_cancel_passthru_case());
	cases.push_back(make_carrier_a_mixed_3stage_case());
	cases.push_back(make_carrier_a_hop_verify_case());
	cases.push_back(make_carrier_a_when_all_case());
	cases.push_back(make_carrier_a_when_all_fast_fail_case());
	cases.push_back(make_carrier_a_race_a_wins_case());
	cases.push_back(make_carrier_a_race_b_wins_case());
#endif
	cases.push_back(make_deadline_scope_arm_disarm_case());
#if CONFLUX_WORK_CARRIER_MODEL_A
	cases.push_back(make_deadline_scope_fast_path_case());
#endif
#if CONFLUX_WORK_CARRIER_MODEL_B
	cases.push_back(make_carrier_b_task_map1_case());
	cases.push_back(make_carrier_b_task_map3_case());
	cases.push_back(make_carrier_b_cancel_passthru_case());
	cases.push_back(make_carrier_b_mixed_3stage_case());
	cases.push_back(make_carrier_b_when_all_case());
#endif
	return cases;
}

} // namespace

int main(
	int argc,
	char **argv) {
	try {
		auto const cfg = parse_args({argv, static_cast<SZ>(argc)});
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
