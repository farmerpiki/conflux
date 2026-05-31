// uring_timeout_bench — batches async_timeout_remove misses through a real ring.
//
// The miss path completes quickly with -ENOENT, so this isolates timeout
// completion ownership without waiting on timer expiry.

#include <liburing.h>

import std;
import conflux.types;
import conflux.work.root;
import conflux.uring;
import conflux.uring.completion;
import conflux.uring.timeout;

import bench_common;
import bench_io_common;

using namespace std::string_view_literals;
namespace root = conflux::work::root;

namespace {

struct Config {
	std::size_t iterations = 20000;
	std::size_t warmup = 1000;
	std::size_t depth = 64;
	std::string config_name = "depth_64";
	bool json_out = false;
};

Config parse_args(
	std::span<char *> args) {
	Config cfg;
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const a = args[i];
		if (a == "--iterations" && i + 1 < args.size()) {
			cfg.iterations = bench_parse_sz(args[++i]);
		} else if (a == "--warmup" && i + 1 < args.size()) {
			cfg.warmup = bench_parse_sz(args[++i]);
		} else if (a == "--depth" && i + 1 < args.size()) {
			cfg.depth = bench_parse_sz(args[++i]);
		} else if (a == "--config-name" && i + 1 < args.size()) {
			cfg.config_name = args[++i];
		} else if (a == "--json") {
			cfg.json_out = true;
		}
	}
	cfg.depth = std::max<std::size_t>(1, cfg.depth);
	return cfg;
}

void submit_nops(
	io_uring &ring,
	std::size_t count) {
	for (std::size_t i = 0; i < count; ++i) {
		::io_uring_sqe *sqe = ::io_uring_get_sqe(&ring);
		if (sqe == nullptr) {
			int const rc = ::io_uring_submit(&ring);
			if (rc < 0) {
				throw std::runtime_error{std::format("io_uring_submit for nop rc={}", rc)};
			}
			sqe = ::io_uring_get_sqe(&ring);
		}
		if (sqe == nullptr) {
			throw std::runtime_error{"io_uring_get_sqe returned null for nop"};
		}
		::io_uring_prep_nop(sqe);
		::io_uring_sqe_set_data64(sqe, static_cast<std::uint64_t>(i));
	}
}

BenchStats bench_nop_submit_cqe(
	io_uring &ring,
	Config const &cfg,
	bool warmup) {
	std::size_t const batches = warmup ? cfg.warmup : cfg.iterations;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		submit_nops(ring, cfg.depth);
		bench_drain_raw_cqes(ring, cfg.depth);
	}
	auto const elapsed = bench_now_ns() - t0;
	std::size_t const ops = batches * cfg.depth;
	return {cfg.config_name, "nop_submit_cqe"sv, ops, elapsed, static_cast<double>(elapsed) / static_cast<double>(ops)};
}

BenchStats bench_timeout_remove_miss(
	io_uring &ring,
	conflux::uring::CompletionTable &completions,
	Config const &cfg,
	bool warmup) {
	std::size_t const batches = warmup ? cfg.warmup : cfg.iterations;
	std::uint64_t tag = 0xC0FFEEULL;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		std::vector<root::Task<void>> tasks;
		tasks.reserve(cfg.depth);
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			tasks.push_back(
				conflux::uring::async_timeout_remove(
					&ring,
					completions,
					[](std::uint32_t slot, std::uint32_t gen) noexcept { return bench_pack_ud(slot, gen); },
					tag++));
		}
		bench_dispatch_cqes(ring, completions, cfg.depth);
		for (auto &task: tasks) {
			auto outcome = root::blocking_join(std::move(task));
			if (outcome.is_failure()) {
				std::rethrow_exception(std::move(outcome).failure().error);
			}
			if (outcome.is_cancelled()) {
				throw std::runtime_error{"timeout_remove miss cancelled"};
			}
		}
	}
	auto const elapsed = bench_now_ns() - t0;
	std::size_t const ops = batches * cfg.depth;
	return {
		cfg.config_name,
		"timeout_remove_miss"sv,
		ops,
		elapsed,
		static_cast<double>(elapsed) / static_cast<double>(ops)};
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"uring_timeout","parser":"standard","configs":[{"name":"depth_64","extra":{"depth":64,"label":"live-kernel-sanity","baseline":"nop_submit_cqe"},"target_ms":500,"max_iterations":20000,"calibration_iterations":4,"args":["--depth","64","--config-name","depth_64","--iterations","0","--warmup","0"]},{"name":"depth_128","extra":{"depth":128,"label":"live-kernel-sanity","baseline":"nop_submit_cqe"},"target_ms":500,"max_iterations":10000,"calibration_iterations":4,"args":["--depth","128","--config-name","depth_128","--iterations","0","--warmup","0"]}]})");

	auto const cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	::io_uring ring{};
	if (::io_uring_queue_init(static_cast<unsigned>(std::max<std::size_t>(256, cfg.depth * 2U)), &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}

	try {
		conflux::uring::CompletionTable completions{cfg.depth * 2U};
		(void)bench_nop_submit_cqe(ring, cfg, true);
		(void)bench_timeout_remove_miss(ring, completions, cfg, true);
		BenchStats stats[]{
			bench_nop_submit_cqe(ring, cfg, false),
			bench_timeout_remove_miss(ring, completions, cfg, false),
		};
		for (std::size_t i = 0; i < std::size(stats); ++i) {
			bench_print(stats[i], cfg.json_out, i == 0);
		}
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_uring_timeout_bench: {}", ex.what());
		::io_uring_queue_exit(&ring);
		return 1;
	}

	::io_uring_queue_exit(&ring);
}
