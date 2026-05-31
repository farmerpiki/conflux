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

BenchStats bench_ready_task_join(
	std::size_t iters) {
	std::uint64_t sink = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		auto [task, source] = root::make_task_source<std::size_t>();
		(void)source.try_set_value(root::Success<std::size_t>{1});
		auto outcome = root::blocking_join(std::move(task));
		if (!outcome.is_success()) {
			std::println(std::cerr, "ready_task_join did not succeed");
			std::exit(1);
		}
		sink += outcome.success().value;
	}
	auto const elapsed = bench_now_ns() - t0;
	if (sink != iters) {
		std::println(std::cerr, "bad ready_task_join sink: {}", sink);
		std::exit(1);
	}
	return {{}, "ready_task_join"sv, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

BenchStats bench_synthetic_cqe_task(
	std::size_t iters) {
	std::uint64_t sink = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
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
	}
	auto const elapsed = bench_now_ns() - t0;
	if (sink != iters) {
		std::println(std::cerr, "bad synthetic_cqe_task sink: {}", sink);
		std::exit(1);
	}
	return {{}, "synthetic_cqe_task"sv, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

BenchStats bench_await_synthetic_cqes(
	std::size_t iters,
	DepthCase dc) {
	std::uint64_t sink = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t iter = 0; iter < iters; ++iter) {
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
	}
	auto const elapsed = bench_now_ns() - t0;
	std::size_t const ops = iters * dc.depth;
	if (sink != ops) {
		std::println(std::cerr, "bad await_synthetic_cqes sink depth {}: {} != {}", dc.depth, sink, ops);
		std::exit(1);
	}
	return {dc.config, "await_synthetic_cqes"sv, ops, elapsed, static_cast<double>(elapsed) / static_cast<double>(ops)};
}

BenchStats bench_synthetic_io_loop(
	std::size_t iters,
	SyntheticLoopCase lc,
	SyntheticIoKind kind,
	std::string_view variant) {
	std::uint64_t sink = 0;
	std::uint64_t ops = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t iter = 0; iter < iters; ++iter) {
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
	}
	auto const elapsed = bench_now_ns() - t0;
	auto const expected = static_cast<std::uint64_t>(iters) * static_cast<std::uint64_t>(lc.bytes);
	if (sink != expected) {
		std::println(std::cerr, "bad {} sink: {} != {}", variant, sink, expected);
		std::exit(1);
	}
	return {lc.config, variant, ops, elapsed, static_cast<double>(elapsed) / static_cast<double>(ops)};
}

BenchStats bench_cancel_before_completion(
	std::size_t iters) {
	std::uint64_t cancelled = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		auto [task, source] = root::make_task_source<std::size_t>(root::SubmitOptions{.enable_cancellation = true});
		task.cancel();
		(void)source.try_set_cancelled();
		auto outcome = root::blocking_join(std::move(task));
		if (!outcome.is_cancelled()) {
			std::println(std::cerr, "cancel_before_completion did not cancel");
			std::exit(1);
		}
		++cancelled;
	}
	auto const elapsed = bench_now_ns() - t0;
	return {
		{},
		"cancel_before_completion"sv,
		cancelled,
		elapsed,
		static_cast<double>(elapsed) / static_cast<double>(cancelled)};
}

BenchStats bench_cancel_after_synthetic_cqe(
	std::size_t iters) {
	std::uint64_t ready = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
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
	}
	auto const elapsed = bench_now_ns() - t0;
	if (ready != iters) {
		std::println(std::cerr, "bad cancel_after_synthetic_cqe sink: {}", ready);
		std::exit(1);
	}
	return {
		{},
		"cancel_after_synthetic_cqe"sv,
		iters,
		elapsed,
		static_cast<double>(elapsed) / static_cast<double>(iters)};
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

	if (cfg.warmup > 0) {
		(void)bench_ready_task_join(cfg.warmup);
		(void)bench_synthetic_cqe_task(cfg.warmup);
		for (auto dc: kDepthCases) {
			(void)bench_await_synthetic_cqes(cfg.warmup, dc);
		}
		for (auto lc: kLoopCases) {
			(void)bench_synthetic_io_loop(cfg.warmup, lc, SyntheticIoKind::file_read, "synthetic_file_read_loop"sv);
			(void)bench_synthetic_io_loop(cfg.warmup, lc, SyntheticIoKind::socket_send, "synthetic_socket_send_loop"sv);
		}
		(void)bench_cancel_before_completion(cfg.warmup);
		(void)bench_cancel_after_synthetic_cqe(cfg.warmup);
	}

	std::vector<BenchStats> stats;
	stats.reserve(4 + kDepthCases.size() + (kLoopCases.size() * 2));
	stats.push_back(bench_ready_task_join(cfg.iterations));
	stats.push_back(bench_synthetic_cqe_task(cfg.iterations));
	for (auto dc: kDepthCases) {
		stats.push_back(bench_await_synthetic_cqes(cfg.iterations, dc));
	}
	for (auto lc: kLoopCases) {
		stats.push_back(
			bench_synthetic_io_loop(cfg.iterations, lc, SyntheticIoKind::file_read, "synthetic_file_read_loop"sv));
		stats.push_back(
			bench_synthetic_io_loop(cfg.iterations, lc, SyntheticIoKind::socket_send, "synthetic_socket_send_loop"sv));
	}
	stats.push_back(bench_cancel_before_completion(cfg.iterations));
	stats.push_back(bench_cancel_after_synthetic_cqe(cfg.iterations));
	for (std::size_t i = 0; i < stats.size(); ++i) {
		bench_print(stats[i], cfg.json_out, i == 0);
	}
}
