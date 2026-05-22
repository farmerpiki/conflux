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

using namespace std::string_view_literals;
namespace root = conflux::work::root;

namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}

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

void pump_all(
	io_uring &ring,
	CompletionTable &completions,
	std::size_t expected) {
	std::size_t completed = 0;
	while (completed < expected) {
		::io_uring_cqe *cqe = nullptr;
		int rc = ::io_uring_submit_and_wait(&ring, 1);
		if (rc >= 0) {
			rc = ::io_uring_peek_cqe(&ring, &cqe);
		}
		if (rc == -EINTR || (rc >= 0 && cqe == nullptr)) {
			continue;
		}
		if (rc < 0 || cqe == nullptr) {
			throw std::runtime_error{std::format("submit_and_wait rc={}", rc)};
		}
		std::array<::io_uring_cqe *, 128> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(&ring, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				auto const *c = batch[static_cast<std::size_t>(i)];
				auto const slot = static_cast<std::uint32_t>(c->user_data & 0xFFFFFFFFU);
				auto const gen = static_cast<std::uint32_t>(c->user_data >> 32U);
				completions.dispatch(slot, gen, c->res, conflux::uring::CqeFlags{c->flags});
			}
			::io_uring_cq_advance(&ring, n);
			completed += n;
		}
	}
}

BenchStats bench_timeout_remove_miss(
	io_uring &ring,
	CompletionTable &completions,
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
					[](std::uint32_t slot, std::uint32_t gen) noexcept { return pack_ud(slot, gen); },
					tag++));
		}
		pump_all(ring, completions, cfg.depth);
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
		R"({"name":"uring_timeout","parser":"standard","configs":[{"name":"depth_64","extra":{"depth":64},"args":["--depth","64","--config-name","depth_64","--iterations","20000","--warmup","1000"]},{"name":"depth_128","extra":{"depth":128},"args":["--depth","128","--config-name","depth_128","--iterations","10000","--warmup","500"]}]})");

	auto const cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	::io_uring ring{};
	if (::io_uring_queue_init(static_cast<unsigned>(std::max<std::size_t>(256, cfg.depth * 2U)), &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}

	try {
		CompletionTable completions{cfg.depth * 2U};
		(void)bench_timeout_remove_miss(ring, completions, cfg, true);
		auto stats = bench_timeout_remove_miss(ring, completions, cfg, false);
		bench_print(stats, cfg.json_out, true);
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_uring_timeout_bench: {}", ex.what());
		::io_uring_queue_exit(&ring);
		return 1;
	}

	::io_uring_queue_exit(&ring);
}
