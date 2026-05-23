// kernel_state_synthetic_bench — no-kernel state-transition baselines for
// transport rows that are otherwise dominated by fd-table, TCP, and
// io_uring_enter round trips.

import std;
import conflux.types;
import conflux.uring.completion;
import conflux.socket_io;
import conflux.net.direct_slot_pool;

import bench_common;

using namespace std::string_view_literals;

namespace {

struct Config {
	std::size_t iterations = 200000;
	std::size_t warmup = 20000;
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
	cfg.depth = std::clamp<std::size_t>(cfg.depth, 1, 4096);
	if (cfg.config_name.empty()) {
		cfg.config_name = std::format("depth_{}", cfg.depth);
	}
	return cfg;
}

struct CloseTicket {
	std::uint32_t slot{};
};

template<class Fn>
BenchStats run_depth_variant(
	Config const &cfg,
	std::string_view variant,
	Fn &&fn) {
	for (std::size_t i = 0; i < cfg.warmup; ++i) {
		fn();
	}

	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < cfg.iterations; ++i) {
		fn();
	}
	auto const elapsed = bench_now_ns() - t0;
	std::size_t const ops = cfg.iterations * cfg.depth;
	return {
		.config = cfg.config_name,
		.variant = variant,
		.iterations = ops,
		.total_ns = elapsed,
		.ns_per_iter = static_cast<double>(elapsed) / static_cast<double>(ops),
	};
}

BenchStats bench_direct_slot_lease(
	Config const &cfg) {
	DirectSlotPool pool{static_cast<std::uint32_t>(cfg.depth)};
	std::vector<std::uint32_t> slots(cfg.depth);
	std::uint64_t sink = 0;
	auto stats = run_depth_variant(cfg, "direct_slot_lease_empty"sv, [&] {
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			auto slot = pool.acquire();
			if (!slot) [[unlikely]] {
				std::terminate();
			}
			slots[i] = *slot;
			sink += static_cast<std::uint64_t>(*slot) + 1U;
		}
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			auto rc = pool.release_empty(slots[i]);
			if (!rc) [[unlikely]] {
				std::terminate();
			}
		}
	});
	if (sink == 0) [[unlikely]] {
		std::terminate();
	}
	return stats;
}

BenchStats bench_direct_slot_close_lifecycle(
	Config const &cfg) {
	DirectSlotPool pool{static_cast<std::uint32_t>(cfg.depth)};
	std::uint64_t sink = 0;
	auto stats = run_depth_variant(cfg, "direct_slot_close_lifecycle"sv, [&] {
		for (std::uint32_t slot = 0; slot < static_cast<std::uint32_t>(cfg.depth); ++slot) {
			if (!pool.adopt_kernel_allocated(slot)) [[unlikely]] {
				std::terminate();
			}
			if (!pool.mark_closing(slot)) [[unlikely]] {
				std::terminate();
			}
			sink += static_cast<std::uint64_t>(slot) + 1U;
			if (!pool.release_closed(slot)) [[unlikely]] {
				std::terminate();
			}
		}
	});
	if (sink == 0) [[unlikely]] {
		std::terminate();
	}
	return stats;
}

BenchStats bench_deferred_close_queue(
	Config const &cfg) {
	DirectSlotPool pool{static_cast<std::uint32_t>(cfg.depth)};
	std::vector<CloseTicket> queue;
	queue.reserve(cfg.depth);
	std::uint64_t sink = 0;
	auto stats = run_depth_variant(cfg, "deferred_close_queue"sv, [&] {
		queue.clear();
		for (std::uint32_t slot = 0; slot < static_cast<std::uint32_t>(cfg.depth); ++slot) {
			if (!pool.adopt_kernel_allocated(slot)) [[unlikely]] {
				std::terminate();
			}
			if (!pool.mark_closing(slot)) [[unlikely]] {
				std::terminate();
			}
			queue.push_back(CloseTicket{.slot = slot});
		}
		for (auto const ticket: queue) {
			sink += static_cast<std::uint64_t>(ticket.slot) + 1U;
			if (!pool.release_closed(ticket.slot)) [[unlikely]] {
				std::terminate();
			}
		}
	});
	if (sink == 0) [[unlikely]] {
		std::terminate();
	}
	return stats;
}

BenchStats bench_generation_advance_alive(
	Config const &cfg) {
	GenerationTable generations{static_cast<std::uint32_t>(cfg.depth)};
	std::uint64_t sink = 0;
	auto stats = run_depth_variant(cfg, "generation_advance_alive"sv, [&] {
		for (std::uint32_t slot = 0; slot < static_cast<std::uint32_t>(cfg.depth); ++slot) {
			auto const gen = generations.advance(slot);
			if (!generations.alive(slot, gen)) [[unlikely]] {
				std::terminate();
			}
			sink += gen;
		}
	});
	if (sink == 0) [[unlikely]] {
		std::terminate();
	}
	return stats;
}

BenchStats bench_generation_stale_reject(
	Config const &cfg) {
	GenerationTable generations{static_cast<std::uint32_t>(cfg.depth)};
	std::vector<std::uint32_t> stale(cfg.depth);
	for (std::uint32_t slot = 0; slot < static_cast<std::uint32_t>(cfg.depth); ++slot) {
		stale[slot] = generations.advance(slot);
		(void)generations.advance(slot);
	}
	std::uint64_t sink = 0;
	auto stats = run_depth_variant(cfg, "generation_stale_reject"sv, [&] {
		for (std::uint32_t slot = 0; slot < static_cast<std::uint32_t>(cfg.depth); ++slot) {
			if (!generations.alive(slot, stale[slot])) {
				++sink;
			}
		}
	});
	if (sink == 0) [[unlikely]] {
		std::terminate();
	}
	return stats;
}

BenchStats bench_completion_dispatch_depth(
	Config const &cfg) {
	CompletionTable completions{cfg.depth};
	std::vector<std::pair<std::uint32_t, std::uint32_t>> refs(cfg.depth);
	std::uint64_t sink = 0;
	auto stats = run_depth_variant(cfg, "completion_dispatch_depth"sv, [&] {
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			refs[i] = completions.reserve([&sink](IoResult r) noexcept {
				sink += static_cast<std::uint64_t>(r.res);
			});
		}
		for (auto const [slot, gen]: refs) {
			completions.dispatch(slot, gen, 1, conflux::uring::CqeFlags{});
		}
	});
	if (sink == 0) [[unlikely]] {
		std::terminate();
	}
	return stats;
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"kernel_state_synthetic","parser":"standard","configs":[{"name":"depth_1","extra":{"depth":1,"label":"micro/user-space"},"target_ms":500,"max_iterations":1000000,"calibration_iterations":8,"args":["--depth","1","--config-name","depth_1","--iterations","0","--warmup","0"]},{"name":"depth_8","extra":{"depth":8,"label":"micro/user-space"},"target_ms":500,"max_iterations":1000000,"calibration_iterations":8,"args":["--depth","8","--config-name","depth_8","--iterations","0","--warmup","0"]},{"name":"depth_32","extra":{"depth":32,"label":"micro/user-space"},"target_ms":500,"max_iterations":500000,"calibration_iterations":8,"args":["--depth","32","--config-name","depth_32","--iterations","0","--warmup","0"]},{"name":"depth_128","extra":{"depth":128,"label":"micro/user-space"},"target_ms":500,"max_iterations":200000,"calibration_iterations":4,"args":["--depth","128","--config-name","depth_128","--iterations","0","--warmup","0"]},{"name":"depth_512","extra":{"depth":512,"label":"micro/user-space"},"target_ms":500,"max_iterations":100000,"calibration_iterations":4,"args":["--depth","512","--config-name","depth_512","--iterations","0","--warmup","0"]}]})");

	auto const cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	BenchStats stats[] = {
		bench_direct_slot_lease(cfg),
		bench_direct_slot_close_lifecycle(cfg),
		bench_deferred_close_queue(cfg),
		bench_generation_advance_alive(cfg),
		bench_generation_stale_reject(cfg),
		bench_completion_dispatch_depth(cfg),
	};
	for (std::size_t i = 0; i < std::size(stats); ++i) {
		bench_print(stats[i], cfg.json_out, i == 0);
	}
}
