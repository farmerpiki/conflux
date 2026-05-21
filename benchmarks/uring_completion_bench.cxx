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
	CompletionTable completions{64};
	std::uint64_t sink = 0;
	for (std::size_t i = 0; i < warmup; ++i) {
		auto [slot, gen] =
			completions.reserve([&sink](IoResult r) noexcept { sink += static_cast<std::uint64_t>(r.res); });
		completions.dispatch(slot, gen, 1, 0);
	}
	if (sink == 0) {
		std::println(std::cerr, "unexpected zero warmup sink");
	}
}

BenchStats bench_small_callback(
	std::size_t iters) {
	CompletionTable completions{64};
	std::uint64_t sink = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		auto [slot, gen] =
			completions.reserve([&sink](IoResult r) noexcept { sink += static_cast<std::uint64_t>(r.res); });
		completions.dispatch(slot, gen, 1, 0);
	}
	auto const elapsed = bench_now_ns() - t0;
	if (sink != iters) {
		std::println(std::cerr, "bad small sink: {}", sink);
		std::exit(1);
	}
	return {{}, "small_callback"sv, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

BenchStats bench_shared_callback(
	std::size_t iters) {
	CompletionTable completions{64};
	auto sink = std::make_shared<std::uint64_t>(0);
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		auto [slot, gen] =
			completions.reserve([sink](IoResult r) noexcept { *sink += static_cast<std::uint64_t>(r.res); });
		completions.dispatch(slot, gen, 1, 0);
	}
	auto const elapsed = bench_now_ns() - t0;
	if (*sink != iters) {
		std::println(std::cerr, "bad shared sink: {}", *sink);
		std::exit(1);
	}
	return {{}, "shared_callback"sv, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

BenchStats bench_large_callback(
	std::size_t iters) {
	CompletionTable completions{64};
	std::uint64_t sink = 0;
	LargeCapture base{.sink = &sink};
	for (std::size_t i = 0; i < base.words.size(); ++i) {
		base.words[i] = i + 1U;
	}
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iters; ++i) {
		auto [slot, gen] = completions.reserve(base);
		completions.dispatch(slot, gen, static_cast<int>(i), 0);
	}
	auto const elapsed = bench_now_ns() - t0;
	if (sink == 0) {
		std::println(std::cerr, "bad large sink");
		std::exit(1);
	}
	return {{}, "large_callback"sv, iters, elapsed, static_cast<double>(elapsed) / static_cast<double>(iters)};
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"uring_completion","parser":"standard","configs":[{"name":"default","extra":{},"args":["--iterations","2000000","--warmup","200000"]}]})");

	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	run_warmup(cfg.warmup);

	BenchStats stats[] = {
		bench_small_callback(cfg.iterations),
		bench_shared_callback(cfg.iterations),
		bench_large_callback(cfg.iterations),
	};
	for (std::size_t i = 0; i < std::size(stats); ++i) {
		bench_print(stats[i], cfg.json_out, i == 0);
	}
}
