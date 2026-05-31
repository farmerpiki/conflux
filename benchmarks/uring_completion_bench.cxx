// uring_completion_bench — isolates CompletionTable reserve/dispatch overhead.
//
// The variants intentionally use copyable callbacks so the same benchmark
// source compiles against the old std::function baseline and the new move-only
// inline-erased candidate.

import std;
import conflux.types;
import conflux.uring.completion;

import bench_common;

using namespace std::string_view_literals;

namespace {

struct DepthCase {
	std::size_t depth{};
	std::string_view config;
	std::string_view variant;
};

constexpr std::array<DepthCase, 5> kDepthCases{
	{
     {1, "depth_1"sv, "multishot_dispatch_depth_1"sv},
     {8, "depth_8"sv, "multishot_dispatch_depth_8"sv},
     {32, "depth_32"sv, "multishot_dispatch_depth_32"sv},
     {128, "depth_128"sv, "multishot_dispatch_depth_128"sv},
     {512, "depth_512"sv, "multishot_dispatch_depth_512"sv},
	 }
};

struct LargeCapture {
	std::array<std::uint64_t, 16> words{};
	std::uint64_t *sink{};

	void operator ()(
		IoResult r) const noexcept {
		*sink += words[static_cast<std::size_t>(r.res) & (words.size() - 1U)];
	}
};

void run_warmup(
	std::size_t warmup) {
	if (warmup == 0) {
		return;
	}
	CompletionTable completions{64};
	std::uint64_t sink = 0;
	for (std::size_t i = 0; i < warmup; ++i) {
		auto [slot, gen] =
			completions.reserve([&sink](IoResult r) noexcept { sink += static_cast<std::uint64_t>(r.res); });
		completions.dispatch(slot, gen, 1, conflux::uring::CqeFlags{});
	}
	if (sink == 0) {
		std::println(std::cerr, "unexpected zero warmup sink");
	}
}

BenchStats bench_small_callback(
	BenchSamplePlan const &plan) {
	CompletionTable completions{64};
	std::uint64_t sink = 0;
	auto stats = bench_measure_batched(
		[&] {
			auto [slot, gen] =
				completions.reserve([&sink](IoResult r) noexcept { sink += static_cast<std::uint64_t>(r.res); });
			completions.dispatch(slot, gen, 1, conflux::uring::CqeFlags{});
		},
		plan);
	if (sink != plan.iterations) {
		std::println(std::cerr, "bad small sink: {}", sink);
		std::exit(1);
	}
	stats.variant = "small_callback"sv;
	return stats;
}

BenchStats bench_shared_callback(
	BenchSamplePlan const &plan) {
	CompletionTable completions{64};
	auto sink = std::make_shared<std::uint64_t>(0);
	auto stats = bench_measure_batched(
		[&] {
			auto [slot, gen] =
				completions.reserve([sink](IoResult r) noexcept { *sink += static_cast<std::uint64_t>(r.res); });
			completions.dispatch(slot, gen, 1, conflux::uring::CqeFlags{});
		},
		plan);
	if (*sink != plan.iterations) {
		std::println(std::cerr, "bad shared sink: {}", *sink);
		std::exit(1);
	}
	stats.variant = "shared_callback"sv;
	return stats;
}

BenchStats bench_large_callback(
	BenchSamplePlan const &plan) {
	CompletionTable completions{64};
	std::uint64_t sink = 0;
	LargeCapture base{.sink = &sink};
	for (std::size_t i = 0; i < base.words.size(); ++i) {
		base.words[i] = i + 1U;
	}
	auto stats = bench_measure_batched(
		[&, i = std::size_t{}]() mutable {
			auto [slot, gen] = completions.reserve(base);
			completions.dispatch(slot, gen, static_cast<int>(i), conflux::uring::CqeFlags{});
			++i;
		},
		plan);
	if (sink == 0) {
		std::println(std::cerr, "bad large sink");
		std::exit(1);
	}
	stats.variant = "large_callback"sv;
	return stats;
}

BenchStats bench_multishot_dispatch_depth(
	BenchSamplePlan const &plan,
	DepthCase dc) {
	auto const depth = dc.depth;
	CompletionTable completions{depth};
	std::uint64_t sink = 0;
	std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;
	entries.reserve(depth);
	for (std::size_t i = 0; i < depth; ++i) {
		auto [slot, gen] = completions.reserve_multishot([&sink](IoResult r) noexcept {
			if (r.res > 0) {
				sink += static_cast<std::uint64_t>(r.res);
			}
		});
		entries.emplace_back(slot, gen);
	}

	auto const flags = conflux::uring::cqe_flags::more;
	auto stats = bench_measure_batched(
		[&] {
			for (auto const [slot, gen]: entries) {
				completions.dispatch(slot, gen, 1, flags);
			}
		},
		plan);
	std::size_t const ops = plan.iterations * depth;
	if (sink != ops) {
		std::println(std::cerr, "bad multishot sink depth {}: {} != {}", depth, sink, ops);
		std::exit(1);
	}
	auto _ = completions.cancel_all();
	stats.config = dc.config;
	stats.variant = dc.variant;
	stats.iterations = ops;
	stats.ns_per_iter /= static_cast<double>(depth);
	return stats;
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"uring_completion","parser":"standard","configs":[{"name":"default","extra":{"label":"micro/user-space","depths":[1,8,32,128,512]},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--iterations","0","--warmup","0"]}]})");

	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	auto const plan = bench_sample_plan(cfg, 200000, 20000);
	run_warmup(plan.warmup_iterations);

	std::vector<BenchStats> stats;
	stats.reserve(3 + kDepthCases.size());
	stats.push_back(bench_small_callback(plan));
	stats.push_back(bench_shared_callback(plan));
	stats.push_back(bench_large_callback(plan));
	for (auto dc: kDepthCases) {
		stats.push_back(bench_multishot_dispatch_depth(plan, dc));
	}
	for (std::size_t i = 0; i < stats.size(); ++i) {
		bench_print(stats[i], cfg.json_out, i == 0);
	}
}
