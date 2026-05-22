// json_storage_bench — parser storage attribution for JSON arena work.
//
// This is an instrumentation benchmark, not a reserve-policy change. Raw NDJSON
// rows include parse storage capacities, arena use, PMR allocation bytes, and
// duplicate-key hash activity so later reserve changes can be attributed.

import std;
import conflux.json;

import bench_common;

using namespace std::string_view_literals;
using namespace conflux::json;

namespace {

class CountingResource final : public std::pmr::memory_resource {
	std::pmr::memory_resource *upstream_;

	void *do_allocate(
		std::size_t bytes,
		std::size_t alignment) override {
		++allocations_;
		bytes_allocated_ += bytes;
		peak_live_bytes_ = std::max(peak_live_bytes_, bytes_live_ += bytes);
		return upstream_->allocate(bytes, alignment);
	}
	void do_deallocate(
		void *p,
		std::size_t bytes,
		std::size_t alignment) override {
		bytes_live_ -= bytes;
		upstream_->deallocate(p, bytes, alignment);
	}
	[[nodiscard]] bool do_is_equal(
		std::pmr::memory_resource const &other) const noexcept override {
		return this == &other;
	}

public:
	explicit CountingResource(
		std::pmr::memory_resource *upstream = std::pmr::new_delete_resource()) noexcept
		: upstream_{upstream} {}

	std::size_t allocations_{};
	std::size_t bytes_allocated_{};
	std::size_t bytes_live_{};
	std::size_t peak_live_bytes_{};
};

struct Config {
	std::size_t iterations = 500;
	std::size_t warmup = 50;
	std::string config_name = "default";
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
		} else if (a == "--config-name" && i + 1 < args.size()) {
			cfg.config_name = args[++i];
		} else if (a == "--json") {
			cfg.json_out = true;
		}
	}
	return cfg;
}

std::string make_plain_sparse_strings(
	std::size_t members) {
	std::string out;
	out.reserve(members * 24 + 2);
	out += '{';
	for (std::size_t i = 0; i < members; ++i) {
		if (i != 0) {
			out += ',';
		}
		out += std::format(R"("field_{}":{})", i, i);
	}
	out += '}';
	return out;
}

std::string make_escape_heavy_strings(
	std::size_t members,
	std::size_t chars_per_value) {
	std::string out;
	out.reserve(members * (chars_per_value * 2 + 32));
	out += '{';
	for (std::size_t i = 0; i < members; ++i) {
		if (i != 0) {
			out += ',';
		}
		out += std::format(R"("field_{}":")", i);
		for (std::size_t j = 0; j < chars_per_value; ++j) {
			out += "\\u0041";
		}
		out += '"';
	}
	out += '}';
	return out;
}

std::string make_duplicate_keys(
	std::size_t unique_members,
	std::size_t duplicate_repeats) {
	std::string out;
	out.reserve((unique_members + duplicate_repeats) * 24 + 2);
	out += '{';
	for (std::size_t i = 0; i < unique_members; ++i) {
		if (i != 0) {
			out += ',';
		}
		out += std::format(R"("field_{}":{})", i, i);
	}
	for (std::size_t i = 0; i < duplicate_repeats; ++i) {
		out += std::format(R"(,"field_{}":{})", i % unique_members, i);
	}
	out += '}';
	return out;
}

struct Sample {
	BenchStats timing;
	JsonParseStorageStats stats;
	std::size_t pmr_allocations{};
	std::size_t pmr_bytes_allocated{};
	std::size_t pmr_peak_live_bytes{};
};

Sample measure_parse(
	std::string_view config,
	std::string_view variant,
	std::string_view corpus,
	JsonParseOptions opts,
	std::size_t iterations) {
	JsonParseStorageStats last_stats{};
	std::size_t last_allocations{};
	std::size_t last_bytes{};
	std::size_t last_peak{};
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iterations; ++i) {
		CountingResource resource;
		auto doc = parse(corpus, opts, &resource);
		if (!doc) {
			throw std::runtime_error{std::format("parse failed for {}: {}", variant, doc.error().message)};
		}
		last_stats = doc->parse_storage_stats();
		last_allocations = resource.allocations_;
		last_bytes = resource.bytes_allocated_;
		last_peak = resource.peak_live_bytes_;
	}
	auto const elapsed = bench_now_ns() - t0;
	return {
		.timing =
			BenchStats{
					   .config = config,
					   .variant = variant,
					   .iterations = iterations,
					   .total_ns = elapsed,
					   .ns_per_iter = static_cast<double>(elapsed) / static_cast<double>(iterations)},
		.stats = last_stats,
		.pmr_allocations = last_allocations,
		.pmr_bytes_allocated = last_bytes,
		.pmr_peak_live_bytes = last_peak
    };
}

void print_sample(
	Sample const &s,
	bool json_out,
	bool first) {
	if (!json_out) {
		bench_print(s.timing, false, first);
		std::println(
			"  input={} string_arena={}/{} pmr_bytes={} dup_promotions={} dup_hits={}",
			s.stats.input_bytes,
			s.stats.string_arena_size,
			s.stats.string_arena_capacity,
			s.pmr_bytes_allocated,
			s.stats.duplicate_hash_promotions,
			s.stats.duplicate_member_hits);
		return;
	}
	std::println(
		"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
		"\"input_bytes\":{},\"nodes_size\":{},\"nodes_capacity\":{},"
		"\"array_children_size\":{},\"array_children_capacity\":{},"
		"\"object_members_size\":{},\"object_members_capacity\":{},"
		"\"string_arena_size\":{},\"string_arena_capacity\":{},\"string_arena_reserve_bytes\":{},"
		"\"pmr_allocations\":{},\"pmr_bytes_allocated\":{},\"pmr_peak_live_bytes\":{},"
		"\"duplicate_hash_promotions\":{},\"duplicate_hash_inserts\":{},\"duplicate_member_hits\":{},"
		"\"first_wins_rollbacks\":{},\"last_wins_updates\":{}}}",
		s.timing.config,
		s.timing.variant,
		s.timing.iterations,
		s.timing.total_ns,
		s.timing.ns_per_iter,
		s.stats.input_bytes,
		s.stats.nodes_size,
		s.stats.nodes_capacity,
		s.stats.array_children_size,
		s.stats.array_children_capacity,
		s.stats.object_members_size,
		s.stats.object_members_capacity,
		s.stats.string_arena_size,
		s.stats.string_arena_capacity,
		s.stats.string_arena_reserve_bytes,
		s.pmr_allocations,
		s.pmr_bytes_allocated,
		s.pmr_peak_live_bytes,
		s.stats.duplicate_hash_promotions,
		s.stats.duplicate_hash_inserts,
		s.stats.duplicate_member_hits,
		s.stats.first_wins_rollbacks,
		s.stats.last_wins_updates);
	(void)first;
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"json_storage","parser":"standard","configs":[{"name":"default","extra":{},"args":["--config-name","default","--iterations","500","--warmup","50"]}]})");

	auto const cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	std::string const sparse = make_plain_sparse_strings(4096);
	std::string const escaped = make_escape_heavy_strings(256, 64);
	std::string const dup = make_duplicate_keys(64, 64);

	JsonParseOptions default_opts;
	JsonParseOptions first_wins_opts;
	first_wins_opts.duplicate_key = DuplicateKeyPolicy::first_wins;
	JsonParseOptions last_wins_opts;
	last_wins_opts.duplicate_key = DuplicateKeyPolicy::last_wins;

	try {
		(void)measure_parse(cfg.config_name, "plain_sparse_strings"sv, sparse, default_opts, cfg.warmup);
		(void)measure_parse(cfg.config_name, "escape_heavy_strings"sv, escaped, default_opts, cfg.warmup);
		(void)measure_parse(cfg.config_name, "duplicate_first_wins"sv, dup, first_wins_opts, cfg.warmup);
		(void)measure_parse(cfg.config_name, "duplicate_last_wins"sv, dup, last_wins_opts, cfg.warmup);

		std::array const samples{
			measure_parse(cfg.config_name, "plain_sparse_strings"sv, sparse, default_opts, cfg.iterations),
			measure_parse(cfg.config_name, "escape_heavy_strings"sv, escaped, default_opts, cfg.iterations),
			measure_parse(cfg.config_name, "duplicate_first_wins"sv, dup, first_wins_opts, cfg.iterations),
			measure_parse(cfg.config_name, "duplicate_last_wins"sv, dup, last_wins_opts, cfg.iterations),
		};
		for (std::size_t i = 0; i < samples.size(); ++i) {
			print_sample(samples[i], cfg.json_out, i == 0);
		}
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_json_storage_bench: {}", ex.what());
		return 1;
	}
}
