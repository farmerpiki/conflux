// synthetic_cqe_coro_bench — no-kernel coroutine/completion isolation rows.
//
// These rows synthesize CQE dispatch through conflux::uring::CompletionTable and root::Task
// sources so coroutine frame allocation, continuation dispatch, task-source
// completion, and callback ownership are visible without a live io_uring trip.

import std;
import conflux.types;
import conflux.work.root;
import conflux.uring.completion;

import bench_common;

using namespace std::string_view_literals;
namespace root = conflux::work::root;

namespace {

struct DepthCase {
	std::size_t depth{};
	std::string_view config;
};

constexpr std::array<DepthCase, 5> kDepthCases{
	{
     {1, "depth_1"sv},
     {8, "depth_8"sv},
     {32, "depth_32"sv},
     {128, "depth_128"sv},
     {512, "depth_512"sv},
	 }
};

struct CompletionEntry {
	std::uint32_t slot{};
	std::uint32_t gen{};
};

enum class SyntheticIoKind : std::uint8_t {
	file_read,
	socket_send,
};

struct SyntheticIoDevice {
	struct Pending {
		std::uint32_t slot{};
		std::uint32_t gen{};
		std::int32_t res{};
	};

	conflux::uring::CompletionTable completions;
	std::deque<Pending> pending;
	std::size_t submitted = 0;
	std::size_t completed = 0;
	SyntheticIoKind kind;

	explicit SyntheticIoDevice(
		std::size_t capacity,
		SyntheticIoKind k)
		: completions{std::max<std::size_t>(capacity, 1)}
		, kind{k} {}

	[[nodiscard]] root::Task<std::size_t> async_op(
		std::size_t bytes) {
		auto [task, source] = root::make_task_source<std::size_t>();
		auto [slot, gen] =
			completions.reserve([source = std::move(source)](conflux::uring::IoResult r) mutable noexcept {
				if (r.res < 0) {
					(void)source.try_set_error(std::make_error_code(std::errc::io_error));
					return;
				}
				(void)source.try_set_value(root::Success<std::size_t>{static_cast<std::size_t>(r.res)});
			});
		pending.push_back(
			Pending{
				.slot = slot,
				.gen = gen,
				.res = static_cast<std::int32_t>(bytes),
			});
		++submitted;
		return std::move(task);
	}

	void complete_one() {
		if (pending.empty()) [[unlikely]] {
			std::println(std::cerr, "synthetic_io_device has no pending completion");
			std::exit(1);
		}
		auto entry = pending.front();
		pending.pop_front();
		completions.dispatch(entry.slot, entry.gen, entry.res, conflux::uring::CqeFlags{});
		++completed;
	}
};

struct SyntheticLoopCase {
	std::string_view config;
	std::size_t bytes;
	std::size_t chunk;
};

constexpr std::array<SyntheticLoopCase, 3> kLoopCases{
	{
     {"64k_4k"sv, 64U * 1024U, 4U * 1024U},
     {"1m_4k"sv, 1U * 1024U * 1024U, 4U * 1024U},
     {"1m_64k"sv, 1U * 1024U * 1024U, 64U * 1024U},
	 }
};

[[nodiscard]] root::Task<std::uint64_t> synthetic_io_loop(
	SyntheticIoDevice &device,
	SyntheticLoopCase lc) {
	std::uint64_t total = 0;
	for (std::size_t off = 0; off < lc.bytes;) {
		auto const n = std::min(lc.chunk, lc.bytes - off);
		total += co_await device.async_op(n);
		off += n;
	}
	co_return total;
}

[[nodiscard]] root::Task<std::uint64_t> await_all(
	std::vector<root::Task<std::size_t>> tasks) {
	std::uint64_t sum = 0;
	for (auto &task: tasks) {
		sum += co_await std::move(task);
	}
	co_return sum;
}

void scale_stats_to_operations(
	BenchStats &stats,
	std::size_t operations_per_iteration) noexcept {
	stats.iterations *= operations_per_iteration;
	stats.batch *= operations_per_iteration;
	stats.ns_per_iter /= static_cast<double>(operations_per_iteration);
}

BenchStats bench_ready_task_join(
	BenchArgs const &args) {
	BenchSamplePlan const plan = bench_sample_plan(args.iterations, args.warmup, args.samples, args.batch);
	std::uint64_t sink = 0;
	auto stats = bench_measure_batched(
		[&] {
			auto [task, source] = root::make_task_source<std::size_t>();
			(void)source.try_set_value(root::Success<std::size_t>{1});
			auto outcome = root::blocking_join(std::move(task));
			if (!outcome.is_success()) {
				std::println(std::cerr, "ready_task_join did not succeed");
				std::exit(1);
			}
			sink += outcome.success().value;
		},
		plan);
	if (sink != plan.iterations + plan.warmup_iterations) {
		std::println(std::cerr, "bad ready_task_join sink: {}", sink);
		std::exit(1);
	}
	stats.variant = "ready_task_join"sv;
	return stats;
}

BenchStats bench_synthetic_cqe_task(
	BenchArgs const &args) {
	BenchSamplePlan const plan = bench_sample_plan(args.iterations, args.warmup, args.samples, args.batch);
	std::uint64_t sink = 0;
	auto stats = bench_measure_batched(
		[&] {
			conflux::uring::CompletionTable completions{1};
			auto [task, source] = root::make_task_source<std::size_t>();
			auto [slot, gen] =
				completions.reserve([source = std::move(source)](conflux::uring::IoResult r) mutable noexcept {
					(void)source.try_set_value(root::Success<std::size_t>{static_cast<std::size_t>(r.res)});
				});
			completions.dispatch(slot, gen, 1, conflux::uring::CqeFlags{});
			auto outcome = root::blocking_join(std::move(task));
			if (!outcome.is_success()) {
				std::println(std::cerr, "synthetic_cqe_task did not succeed");
				std::exit(1);
			}
			sink += outcome.success().value;
		},
		plan);
	if (sink != plan.iterations + plan.warmup_iterations) {
		std::println(std::cerr, "bad synthetic_cqe_task sink: {}", sink);
		std::exit(1);
	}
	stats.variant = "synthetic_cqe_task"sv;
	return stats;
}

BenchStats bench_await_synthetic_cqes(
	BenchArgs const &args,
	DepthCase dc) {
	BenchSamplePlan const plan = bench_sample_plan(args.iterations, args.warmup, args.samples, args.batch);
	std::uint64_t sink = 0;
	auto stats = bench_measure_batched(
		[&] {
			conflux::uring::CompletionTable completions{dc.depth};
			std::vector<CompletionEntry> entries;
			std::vector<root::Task<std::size_t>> tasks;
			entries.reserve(dc.depth);
			tasks.reserve(dc.depth);
			for (std::size_t i = 0; i < dc.depth; ++i) {
				auto [task, source] = root::make_task_source<std::size_t>();
				auto [slot, gen] =
					completions.reserve([source = std::move(source)](conflux::uring::IoResult r) mutable noexcept {
						(void)source.try_set_value(root::Success<std::size_t>{static_cast<std::size_t>(r.res)});
					});
				entries.push_back({.slot = slot, .gen = gen});
				tasks.push_back(std::move(task));
			}

			auto waiter = await_all(std::move(tasks));
			for (auto const &entry: entries) {
				completions.dispatch(entry.slot, entry.gen, 1, conflux::uring::CqeFlags{});
			}
			auto outcome = root::blocking_join(std::move(waiter));
			if (!outcome.is_success()) {
				std::println(std::cerr, "await_synthetic_cqes did not succeed at depth {}", dc.depth);
				std::exit(1);
			}
			sink += outcome.success().value;
		},
		plan);
	std::size_t const ops = (plan.iterations + plan.warmup_iterations) * dc.depth;
	if (sink != ops) {
		std::println(std::cerr, "bad await_synthetic_cqes sink depth {}: {} != {}", dc.depth, sink, ops);
		std::exit(1);
	}
	stats.config = dc.config;
	stats.variant = "await_synthetic_cqes"sv;
	scale_stats_to_operations(stats, dc.depth);
	return stats;
}

BenchStats bench_synthetic_io_loop(
	BenchArgs const &args,
	SyntheticLoopCase lc,
	SyntheticIoKind kind,
	std::string_view variant) {
	BenchSamplePlan const plan = bench_sample_plan(args.iterations, args.warmup, args.samples, args.batch);
	std::uint64_t sink = 0;
	std::uint64_t ops = 0;
	auto stats = bench_measure_batched(
		[&] {
			SyntheticIoDevice device{2, kind};
			auto task = synthetic_io_loop(device, lc);
			while (!task.control().ready()) {
				device.complete_one();
			}
			auto outcome = root::blocking_join(std::move(task));
			if (!outcome.is_success()) {
				std::println(std::cerr, "{} failed", variant);
				std::exit(1);
			}
			sink += outcome.success().value;
			ops += device.completed;
		},
		plan);
	auto const expected =
		static_cast<std::uint64_t>(plan.iterations + plan.warmup_iterations) * static_cast<std::uint64_t>(lc.bytes);
	if (sink != expected) {
		std::println(std::cerr, "bad {} sink: {} != {}", variant, sink, expected);
		std::exit(1);
	}
	std::size_t const ops_per_iteration = (lc.bytes + lc.chunk - 1U) / lc.chunk;
	stats.config = lc.config;
	stats.variant = variant;
	scale_stats_to_operations(stats, ops_per_iteration);
	if (ops != (plan.iterations + plan.warmup_iterations) * ops_per_iteration) {
		std::println(std::cerr, "bad {} ops: {}", variant, ops);
		std::exit(1);
	}
	return stats;
}

BenchStats bench_cancel_before_completion(
	BenchArgs const &args) {
	BenchSamplePlan const plan = bench_sample_plan(args.iterations, args.warmup, args.samples, args.batch);
	std::uint64_t cancelled = 0;
	auto stats = bench_measure_batched(
		[&] {
			auto [task, source] = root::make_task_source<std::size_t>(root::SubmitOptions{.enable_cancellation = true});
			task.cancel();
			(void)source.try_set_cancelled();
			auto outcome = root::blocking_join(std::move(task));
			if (!outcome.is_cancelled()) {
				std::println(std::cerr, "cancel_before_completion did not cancel");
				std::exit(1);
			}
			++cancelled;
		},
		plan);
	if (cancelled != plan.iterations + plan.warmup_iterations) {
		std::println(std::cerr, "bad cancel_before_completion sink: {}", cancelled);
		std::exit(1);
	}
	stats.variant = "cancel_before_completion"sv;
	return stats;
}

BenchStats bench_cancel_after_synthetic_cqe(
	BenchArgs const &args) {
	BenchSamplePlan const plan = bench_sample_plan(args.iterations, args.warmup, args.samples, args.batch);
	std::uint64_t ready = 0;
	auto stats = bench_measure_batched(
		[&] {
			conflux::uring::CompletionTable completions{1};
			auto [task, source] = root::make_task_source<std::size_t>(root::SubmitOptions{.enable_cancellation = true});
			auto [slot, gen] =
				completions.reserve([source = std::move(source)](conflux::uring::IoResult r) mutable noexcept {
					(void)source.try_set_value(root::Success<std::size_t>{static_cast<std::size_t>(r.res)});
				});
			completions.dispatch(slot, gen, 1, conflux::uring::CqeFlags{});
			task.cancel();
			auto outcome = root::blocking_join(std::move(task));
			if (!outcome.is_success()) {
				std::println(std::cerr, "cancel_after_synthetic_cqe did not preserve ready success");
				std::exit(1);
			}
			ready += outcome.success().value;
		},
		plan);
	if (ready != plan.iterations + plan.warmup_iterations) {
		std::println(std::cerr, "bad cancel_after_synthetic_cqe sink: {}", ready);
		std::exit(1);
	}
	stats.variant = "cancel_after_synthetic_cqe"sv;
	return stats;
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"synthetic_cqe_coro","parser":"standard","configs":[{"name":"default","extra":{"label":"micro/user-space","depths":[1,8,32,128,512],"loops":["64k_4k","1m_4k","1m_64k"],"cancellation":true},"target_ms":500,"max_iterations":200000,"calibration_iterations":8,"args":["--iterations","0","--warmup","0"]}]})");

	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});

	std::vector<BenchStats> stats;
	stats.reserve(4 + kDepthCases.size() + (kLoopCases.size() * 2));
	stats.push_back(bench_ready_task_join(cfg));
	stats.push_back(bench_synthetic_cqe_task(cfg));
	for (auto dc: kDepthCases) {
		stats.push_back(bench_await_synthetic_cqes(cfg, dc));
	}
	for (auto lc: kLoopCases) {
		stats.push_back(bench_synthetic_io_loop(cfg, lc, SyntheticIoKind::file_read, "synthetic_file_read_loop"sv));
		stats.push_back(bench_synthetic_io_loop(cfg, lc, SyntheticIoKind::socket_send, "synthetic_socket_send_loop"sv));
	}
	stats.push_back(bench_cancel_before_completion(cfg));
	stats.push_back(bench_cancel_after_synthetic_cqe(cfg));
	for (std::size_t i = 0; i < stats.size(); ++i) {
		bench_print(stats[i], cfg.json_out, i == 0);
	}
}
