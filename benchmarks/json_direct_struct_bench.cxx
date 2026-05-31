// json_direct_struct_bench — focused bottleneck matrix for direct typed JSON decode.
//
// This target deliberately avoids file/socket I/O and DOM traversal. Rows isolate
// direct-to-struct reader costs: member order, wide-object key lookup, unknown
// member policy, duplicate-key policy, number lexing, fixed numeric arrays, and
// string decode paths.

#include <atomic>
#include <cstdlib>
#include <new>

import std;
import conflux.json;
import bench_common;

using namespace conflux::json;

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::uint64_t> g_alloc_count{0};
std::atomic<std::uint64_t> g_alloc_bytes{0};

} // namespace

void *operator new(
	std::size_t size) {
	if (void *p = std::malloc(size)) {
		if (g_count_allocations.load(std::memory_order_relaxed)) {
			g_alloc_count.fetch_add(1, std::memory_order_relaxed);
			g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
		}
		return p;
	}
	throw std::bad_alloc{};
}
void *operator new[](
	std::size_t size) {
	return ::operator new(size);
}
void operator delete(
	void *p) noexcept {
	std::free(p);
}
void operator delete[](
	void *p) noexcept {
	::operator delete(p);
}
void operator delete(
	void *p,
	std::size_t) noexcept {
	::operator delete(p);
}
void operator delete[](
	void *p,
	std::size_t) noexcept {
	::operator delete(p);
}

namespace {

struct AllocBenchStats {
	BenchStats timing;
	double allocations_per_iter{};
	double allocated_bytes_per_iter{};
};

struct Config {
	std::size_t iterations{500};
	std::size_t warmup{100};
	std::string config_name{"default"};
	bool json_out{false};
	std::vector<std::string> filters;
};

[[nodiscard]] Config parse_args(
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
		} else if (a == "--filter" && i + 1 < args.size()) {
			cfg.filters.emplace_back(args[++i]);
		}
	}
	return cfg;
}

[[nodiscard]] bool should_run(
	Config const &cfg,
	std::string_view variant) {
	return bench_matches_filter(std::span<std::string const>{cfg.filters}, variant);
}

template<typename F>
[[nodiscard]] AllocBenchStats measure_alloc(
	F &&fn,
	std::size_t warmup,
	std::size_t iters,
	std::size_t batch,
	std::size_t bytes) {
	for (std::size_t i = 0; i < warmup * batch; ++i) {
		fn();
	}
	std::vector<std::uint64_t> samples;
	samples.reserve(iters);
	std::uint64_t total_ns = 0;
	std::uint64_t total_allocs = 0;
	std::uint64_t total_bytes = 0;
	for (std::size_t i = 0; i < iters; ++i) {
		g_alloc_count.store(0, std::memory_order_relaxed);
		g_alloc_bytes.store(0, std::memory_order_relaxed);
		g_count_allocations.store(true, std::memory_order_relaxed);
		std::uint64_t const t0 = bench_now_ns();
		for (std::size_t j = 0; j < batch; ++j) {
			fn();
		}
		std::uint64_t const elapsed = bench_now_ns() - t0;
		g_count_allocations.store(false, std::memory_order_relaxed);
		total_ns += elapsed;
		total_allocs += g_alloc_count.load(std::memory_order_relaxed);
		total_bytes += g_alloc_bytes.load(std::memory_order_relaxed);
		samples.push_back(elapsed);
	}
	std::ranges::sort(samples);
	double const median_ns = static_cast<double>(samples[iters / 2]) / static_cast<double>(batch);
	double const mbs =
		bytes > 0 && median_ns > 0.0 ? static_cast<double>(bytes) / (median_ns / 1e9) / (1024.0 * 1024.0) : 0.0;
	double const denom = static_cast<double>(iters * batch);
	return {
		.timing =
			{
					 .iterations = iters * batch,
					 .total_ns = total_ns,
					 .ns_per_iter = median_ns,
					 .throughput = mbs,
					 },
		.allocations_per_iter = static_cast<double>(total_allocs) / denom,
		.allocated_bytes_per_iter = static_cast<double>(total_bytes) / denom,
	};
}

void print_row(
	Config const &cfg,
	std::string_view variant,
	AllocBenchStats s) {
	s.timing.config = cfg.config_name;
	s.timing.variant = variant;
	if (cfg.json_out) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"mb_per_s\":{:.2f},\"allocations_per_iter\":{:.2f},\"allocated_bytes_per_iter\":{:.2f}}}",
			s.timing.config,
			s.timing.variant,
			s.timing.iterations,
			s.timing.total_ns,
			s.timing.ns_per_iter,
			s.timing.throughput,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter);
	} else {
		std::println(
			"[json-direct-struct-bench] {:<52} {:>10.1f} ns  {:>8.1f} MB/s  {:>6.2f} allocs  {:>8.1f} B",
			variant,
			s.timing.ns_per_iter,
			s.timing.throughput,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter);
	}
}

// Batch many decode calls inside each timed window so the cost of the two
// bench_now_ns() calls per sample is amortized to noise. On hosts whose kernel
// clocksource is not TSC (e.g. acpi_pm fallback), a single clock_gettime can
// cost microseconds, which would otherwise dominate per-iteration medians.
inline constexpr std::size_t kRowBatch = 256;

template<class F>
void run_row(
	Config const &cfg,
	std::string_view variant,
	std::size_t bytes,
	F &&fn,
	std::size_t batch = kRowBatch) {
	if (!should_run(cfg, variant)) {
		return;
	}
	std::size_t const iters = cfg.iterations == 0 ? 500 : cfg.iterations;
	std::size_t const warmup = cfg.warmup == 0 ? 100 : cfg.warmup;
	print_row(cfg, variant, measure_alloc(std::forward<F>(fn), warmup, iters, batch, bytes));
}

template<class T>
void require_ok(
	std::expected<T, JsonError> value) {
	if (!value) {
		throw std::runtime_error{value.error().message};
	}
}

template<class T>
void require_error(
	std::expected<T, JsonError> value) {
	if (value) {
		throw std::runtime_error{"expected direct decode to reject benchmark fixture"};
	}
}

struct Medium {
	std::int64_t id{};
	std::int64_t count{};
	double score{};
	bool active{};
	std::string name;
	std::string tag;
	std::optional<std::int64_t> limit{};
	std::vector<std::int64_t> values;
};

struct StringsOwned {
	std::string short_plain;
	std::string short_escaped;
	std::string long_plain;
	std::string long_escaped;
};

struct NumericScalars {
	std::int64_t i{};
	std::uint64_t u{};
	double integer_form{};
	double fixed_form{};
	double scientific_form{};
	double negative_form{};
};

struct PointCloud {
	std::vector<std::array<double, 3>> points;
};

struct Wide64 {
	std::int64_t f00{};
	std::int64_t f01{};
	std::int64_t f02{};
	std::int64_t f03{};
	std::int64_t f04{};
	std::int64_t f05{};
	std::int64_t f06{};
	std::int64_t f07{};
	std::int64_t f08{};
	std::int64_t f09{};
	std::int64_t f10{};
	std::int64_t f11{};
	std::int64_t f12{};
	std::int64_t f13{};
	std::int64_t f14{};
	std::int64_t f15{};
	std::int64_t f16{};
	std::int64_t f17{};
	std::int64_t f18{};
	std::int64_t f19{};
	std::int64_t f20{};
	std::int64_t f21{};
	std::int64_t f22{};
	std::int64_t f23{};
	std::int64_t f24{};
	std::int64_t f25{};
	std::int64_t f26{};
	std::int64_t f27{};
	std::int64_t f28{};
	std::int64_t f29{};
	std::int64_t f30{};
	std::int64_t f31{};
	std::int64_t f32{};
	std::int64_t f33{};
	std::int64_t f34{};
	std::int64_t f35{};
	std::int64_t f36{};
	std::int64_t f37{};
	std::int64_t f38{};
	std::int64_t f39{};
	std::int64_t f40{};
	std::int64_t f41{};
	std::int64_t f42{};
	std::int64_t f43{};
	std::int64_t f44{};
	std::int64_t f45{};
	std::int64_t f46{};
	std::int64_t f47{};
	std::int64_t f48{};
	std::int64_t f49{};
	std::int64_t f50{};
	std::int64_t f51{};
	std::int64_t f52{};
	std::int64_t f53{};
	std::int64_t f54{};
	std::int64_t f55{};
	std::int64_t f56{};
	std::int64_t f57{};
	std::int64_t f58{};
	std::int64_t f59{};
	std::int64_t f60{};
	std::int64_t f61{};
	std::int64_t f62{};
	std::int64_t f63{};
};

} // namespace

template<>
struct conflux::json::JsonMembers<Medium> {
	static constexpr auto members() {
		return std::tuple{
			json_member("id", &Medium::id),
			json_member("count", &Medium::count),
			json_member("score", &Medium::score),
			json_member("active", &Medium::active),
			json_member("name", &Medium::name),
			json_member("tag", &Medium::tag),
			json_member("limit", &Medium::limit),
			json_member("values", &Medium::values),
		};
	}
	static constexpr std::string_view type_name() { return "Medium"; }
};

template<>
struct conflux::json::JsonMembers<StringsOwned> {
	static constexpr auto members() {
		return std::tuple{
			json_member("short_plain", &StringsOwned::short_plain),
			json_member("short_escaped", &StringsOwned::short_escaped),
			json_member("long_plain", &StringsOwned::long_plain),
			json_member("long_escaped", &StringsOwned::long_escaped),
		};
	}
	static constexpr std::string_view type_name() { return "StringsOwned"; }
};

template<>
struct conflux::json::JsonMembers<NumericScalars> {
	static constexpr auto members() {
		return std::tuple{
			json_member("i", &NumericScalars::i),
			json_member("u", &NumericScalars::u),
			json_member("integer_form", &NumericScalars::integer_form),
			json_member("fixed_form", &NumericScalars::fixed_form),
			json_member("scientific_form", &NumericScalars::scientific_form),
			json_member("negative_form", &NumericScalars::negative_form),
		};
	}
	static constexpr std::string_view type_name() { return "NumericScalars"; }
};

template<>
struct conflux::json::JsonMembers<PointCloud> {
	static constexpr auto members() { return std::tuple{json_member("points", &PointCloud::points)}; }
	static constexpr std::string_view type_name() { return "PointCloud"; }
};

template<>
struct conflux::json::JsonMembers<Wide64> {
	static constexpr auto members() {
		return std::tuple{
			json_member("f00", &Wide64::f00), json_member("f01", &Wide64::f01), json_member("f02", &Wide64::f02),
			json_member("f03", &Wide64::f03), json_member("f04", &Wide64::f04), json_member("f05", &Wide64::f05),
			json_member("f06", &Wide64::f06), json_member("f07", &Wide64::f07), json_member("f08", &Wide64::f08),
			json_member("f09", &Wide64::f09), json_member("f10", &Wide64::f10), json_member("f11", &Wide64::f11),
			json_member("f12", &Wide64::f12), json_member("f13", &Wide64::f13), json_member("f14", &Wide64::f14),
			json_member("f15", &Wide64::f15), json_member("f16", &Wide64::f16), json_member("f17", &Wide64::f17),
			json_member("f18", &Wide64::f18), json_member("f19", &Wide64::f19), json_member("f20", &Wide64::f20),
			json_member("f21", &Wide64::f21), json_member("f22", &Wide64::f22), json_member("f23", &Wide64::f23),
			json_member("f24", &Wide64::f24), json_member("f25", &Wide64::f25), json_member("f26", &Wide64::f26),
			json_member("f27", &Wide64::f27), json_member("f28", &Wide64::f28), json_member("f29", &Wide64::f29),
			json_member("f30", &Wide64::f30), json_member("f31", &Wide64::f31), json_member("f32", &Wide64::f32),
			json_member("f33", &Wide64::f33), json_member("f34", &Wide64::f34), json_member("f35", &Wide64::f35),
			json_member("f36", &Wide64::f36), json_member("f37", &Wide64::f37), json_member("f38", &Wide64::f38),
			json_member("f39", &Wide64::f39), json_member("f40", &Wide64::f40), json_member("f41", &Wide64::f41),
			json_member("f42", &Wide64::f42), json_member("f43", &Wide64::f43), json_member("f44", &Wide64::f44),
			json_member("f45", &Wide64::f45), json_member("f46", &Wide64::f46), json_member("f47", &Wide64::f47),
			json_member("f48", &Wide64::f48), json_member("f49", &Wide64::f49), json_member("f50", &Wide64::f50),
			json_member("f51", &Wide64::f51), json_member("f52", &Wide64::f52), json_member("f53", &Wide64::f53),
			json_member("f54", &Wide64::f54), json_member("f55", &Wide64::f55), json_member("f56", &Wide64::f56),
			json_member("f57", &Wide64::f57), json_member("f58", &Wide64::f58), json_member("f59", &Wide64::f59),
			json_member("f60", &Wide64::f60), json_member("f61", &Wide64::f61), json_member("f62", &Wide64::f62),
			json_member("f63", &Wide64::f63)};
	}
	static constexpr std::string_view type_name() { return "Wide64"; }
};

namespace {

enum class OrderShape : std::uint8_t {
	declaration,
	reverse,
	evens_then_odds,
	shuffled,
};

[[nodiscard]] std::vector<std::size_t> order_indices(
	std::size_t n,
	OrderShape shape) {
	std::vector<std::size_t> order;
	order.reserve(n);
	switch (shape) {
	case OrderShape::declaration:
		for (std::size_t i = 0; i < n; ++i) {
			order.push_back(i);
		}
		break;
	case OrderShape::reverse:
		for (std::size_t i = n; i > 0; --i) {
			order.push_back(i - 1);
		}
		break;
	case OrderShape::evens_then_odds:
		for (std::size_t i = 0; i < n; i += 2) {
			order.push_back(i);
		}
		for (std::size_t i = 1; i < n; i += 2) {
			order.push_back(i);
		}
		break;
	case OrderShape::shuffled:
		for (std::size_t i = 0; i < n; ++i) {
			order.push_back(i);
		}
		for (std::size_t i = 0; i < n; ++i) {
			std::size_t const j = (i * 17 + 23) % n;
			std::swap(order[i], order[j]);
		}
		break;
	}
	return order;
}

[[nodiscard]] std::string make_medium_json(
	OrderShape shape = OrderShape::declaration) {
	std::array<std::pair<std::string_view, std::string_view>, 8> const fields{
		{
         {"id", "7"},
         {"count", "42"},
         {"score", "12.5"},
         {"active", "true"},
         {"name", R"("bench")"},
         {"tag", R"("direct")"},
         {"limit", "64"},
         {"values", "[1,2,3,4,5,6,7,8]"},
		 }
    };
	std::string out;
	out.reserve(160);
	out += '{';
	bool first = true;
	for (std::size_t idx: order_indices(fields.size(), shape)) {
		if (!first) {
			out += ',';
		}
		first = false;
		out += '"';
		out += fields[idx].first;
		out += "\":";
		out += fields[idx].second;
	}
	out += '}';
	return out;
}

[[nodiscard]] std::string make_wide_json(
	OrderShape shape,
	bool interleave_unknown = false,
	bool tail_unknown = false) {
	std::string out;
	out.reserve(1800);
	out += '{';
	bool first = true;
	auto sep = [&] {
		if (!first) {
			out += ',';
		}
		first = false;
	};
	for (std::size_t i: order_indices(64, shape)) {
		sep();
		out += std::format(R"("f{:02}":{})", i, i);
		if (interleave_unknown && i % 4 == 0) {
			sep();
			out += std::format(R"("extra_{}":{})", i, i);
		}
	}
	if (tail_unknown) {
		for (std::size_t i = 0; i < 16; ++i) {
			sep();
			out += std::format(R"("tail_extra_{}":{})", i, i);
		}
	}
	out += '}';
	return out;
}

[[nodiscard]] std::string make_duplicate_medium_json(
	std::string_view duplicate_member) {
	if (duplicate_member == "name") {
		return R"({"id":7,"count":42,"score":12.5,"active":true,"name":"first","name":"last","tag":"direct","limit":64,"values":[1,2,3,4]})";
	}
	if (duplicate_member == "values") {
		return R"({"id":7,"count":42,"score":12.5,"active":true,"name":"bench","tag":"direct","limit":64,"values":[1,2,3,4],"values":[9,8,7]})";
	}
	if (duplicate_member == "unknown") {
		return R"({"id":7,"count":42,"score":12.5,"active":true,"extra":1,"extra":2,"name":"bench","tag":"direct","limit":64,"values":[1,2,3,4]})";
	}
	return {};
}

[[nodiscard]] std::string make_json5_duplicate_medium_json() {
	return R"({id:7,count:42,score:12.5,active:true,name:'first',name:'last',tag:'direct',limit:64,values:[1,2,3,4,],})";
}

[[nodiscard]] std::string make_numeric_scalars_json() {
	return R"({"i":-123456789,"u":1844674407370955161,"integer_form":123456,"fixed_form":1234.56789,"scientific_form":-1.25e+42,"negative_form":-9876.54321})";
}

enum class NumberShape : std::uint8_t {
	integers,
	fixed_decimal,
	scientific,
	mixed,
};

[[nodiscard]] std::string point_value(
	std::size_t i,
	std::size_t component,
	NumberShape shape) {
	double const base = static_cast<double>(i * 3 + component + 1);
	switch (shape) {
	case NumberShape::integers     : return std::to_string(static_cast<std::int64_t>(base));
	case NumberShape::fixed_decimal: return std::format("{:.6f}", base + 0.125);
	case NumberShape::scientific   : return std::format("{:.6e}", base + 0.125);
	case NumberShape::mixed:
		switch ((i + component) % 4) {
		case 0 : return std::to_string(static_cast<std::int64_t>(base));
		case 1 : return std::format("{:.3f}", base + 0.5);
		case 2 : return std::format("{:.4e}", base + 0.25);
		case 3 : return std::format("-{:.3f}", base + 0.75);
		default: break;
		}
	}
	return "0";
}

[[nodiscard]] std::string make_point_cloud_json(
	std::size_t points,
	NumberShape shape,
	bool pretty = false) {
	std::string out;
	out.reserve(points * 48 + 32);
	out += pretty ? "{\n  \"points\": [\n" : R"({"points":[)";
	for (std::size_t i = 0; i < points; ++i) {
		if (i != 0) {
			out += pretty ? ",\n" : ",";
		}
		if (pretty) {
			out += "    ";
		}
		out += '[';
		out += point_value(i, 0, shape);
		out += ',';
		out += point_value(i, 1, shape);
		out += ',';
		out += point_value(i, 2, shape);
		out += ']';
	}
	out += pretty ? "\n  ]\n}" : "]}";
	return out;
}

[[nodiscard]] std::string repeat_ascii(
	std::size_t n) {
	std::string s;
	s.reserve(n);
	for (std::size_t i = 0; i < n; ++i) {
		s += static_cast<char>('a' + (i % 26));
	}
	return s;
}

[[nodiscard]] std::string make_strings_json(
	bool escaped_heavy) {
	std::string const long_plain = repeat_ascii(4096);
	std::string long_escaped;
	long_escaped.reserve(4096 * 2);
	for (std::size_t i = 0; i < 1024; ++i) {
		long_escaped += escaped_heavy ? R"(\u0041\n)" : "ab";
	}
	return std::format(
		R"({{"short_plain":"alpha","short_escaped":"line\nquote\"","long_plain":"{}","long_escaped":"{}"}})",
		long_plain,
		long_escaped);
}

void bench_order_matrix(
	Config const &cfg) {
	std::string const medium_decl = make_medium_json(OrderShape::declaration);
	std::string const medium_reverse = make_medium_json(OrderShape::reverse);
	std::string const medium_evens = make_medium_json(OrderShape::evens_then_odds);
	std::string const medium_shuffle = make_medium_json(OrderShape::shuffled);
	std::string const wide_decl = make_wide_json(OrderShape::declaration);
	std::string const wide_reverse = make_wide_json(OrderShape::reverse);
	std::string const wide_evens = make_wide_json(OrderShape::evens_then_odds);
	std::string const wide_shuffle = make_wide_json(OrderShape::shuffled);
	std::string const wide_unknown_interleave = make_wide_json(OrderShape::shuffled, true, false);
	std::string const wide_unknown_tail = make_wide_json(OrderShape::declaration, false, true);
	JsonDecodeOptions ignore_unknown;
	ignore_unknown.unknown_members = UnknownMemberPolicy::ignore;
	JsonDecodeOptions reject_unknown;
	reject_unknown.unknown_members = UnknownMemberPolicy::reject;

	run_row(cfg, "order/medium/declaration", medium_decl.size(), [&] {
		require_ok(decode_borrowed<Medium>(medium_decl));
	});
	run_row(cfg, "order/medium/reverse", medium_reverse.size(), [&] {
		require_ok(decode_borrowed<Medium>(medium_reverse));
	});
	run_row(cfg, "order/medium/evens_then_odds", medium_evens.size(), [&] {
		require_ok(decode_borrowed<Medium>(medium_evens));
	});
	run_row(cfg, "order/medium/shuffled", medium_shuffle.size(), [&] {
		require_ok(decode_borrowed<Medium>(medium_shuffle));
	});
	run_row(cfg, "order/wide64/declaration", wide_decl.size(), [&] { require_ok(decode_borrowed<Wide64>(wide_decl)); });
	run_row(cfg, "order/wide64/reverse", wide_reverse.size(), [&] {
		require_ok(decode_borrowed<Wide64>(wide_reverse));
	});
	run_row(cfg, "order/wide64/evens_then_odds", wide_evens.size(), [&] {
		require_ok(decode_borrowed<Wide64>(wide_evens));
	});
	run_row(cfg, "order/wide64/shuffled", wide_shuffle.size(), [&] {
		require_ok(decode_borrowed<Wide64>(wide_shuffle));
	});
	run_row(cfg, "unknown/wide64/interleaved_ignore", wide_unknown_interleave.size(), [&] {
		require_ok(decode_borrowed<Wide64>(wide_unknown_interleave, {}, ignore_unknown));
	});
	run_row(cfg, "unknown/wide64/tail_ignore", wide_unknown_tail.size(), [&] {
		require_ok(decode_borrowed<Wide64>(wide_unknown_tail, {}, ignore_unknown));
	});
	run_row(cfg, "unknown/wide64/interleaved_reject", wide_unknown_interleave.size(), [&] {
		require_error(decode_borrowed<Wide64>(wide_unknown_interleave, {}, reject_unknown));
	});
}

void bench_duplicate_matrix(
	Config const &cfg) {
	std::string const dup_name = make_duplicate_medium_json("name");
	std::string const dup_values = make_duplicate_medium_json("values");
	std::string const dup_unknown = make_duplicate_medium_json("unknown");
	std::string const dup_json5 = make_json5_duplicate_medium_json();
	JsonParseOptions reject;
	reject.duplicate_key = DuplicateKeyPolicy::reject;
	JsonParseOptions first;
	first.duplicate_key = DuplicateKeyPolicy::first_wins;
	JsonParseOptions last;
	last.duplicate_key = DuplicateKeyPolicy::last_wins;
	JsonParseOptions json5_reject = reject;
	json5_reject.mode = ParseMode::json5;
	JsonParseOptions json5_last = last;
	json5_last.mode = ParseMode::json5;
	JsonDecodeOptions ignore_unknown;
	ignore_unknown.unknown_members = UnknownMemberPolicy::ignore;

	run_row(cfg, "duplicate/name/reject", dup_name.size(), [&] {
		require_error(decode_borrowed<Medium>(dup_name, reject));
	});
	run_row(cfg, "duplicate/name/first_wins", dup_name.size(), [&] {
		require_ok(decode_borrowed<Medium>(dup_name, first));
	});
	run_row(cfg, "duplicate/name/last_wins", dup_name.size(), [&] {
		require_ok(decode_borrowed<Medium>(dup_name, last));
	});
	run_row(cfg, "duplicate/vector/first_wins", dup_values.size(), [&] {
		require_ok(decode_borrowed<Medium>(dup_values, first));
	});
	run_row(cfg, "duplicate/vector/last_wins", dup_values.size(), [&] {
		require_ok(decode_borrowed<Medium>(dup_values, last));
	});
	run_row(cfg, "duplicate/unknown/ignore", dup_unknown.size(), [&] {
		require_ok(decode_borrowed<Medium>(dup_unknown, reject, ignore_unknown));
	});
	run_row(cfg, "duplicate/json5/name/reject", dup_json5.size(), [&] {
		require_error(decode_borrowed<Medium>(dup_json5, json5_reject));
	});
	run_row(cfg, "duplicate/json5/name/last_wins", dup_json5.size(), [&] {
		require_ok(decode_borrowed<Medium>(dup_json5, json5_last));
	});
}

void bench_numeric_matrix(
	Config const &cfg) {
	std::string const scalars = make_numeric_scalars_json();
	std::string const points_i = make_point_cloud_json(256, NumberShape::integers);
	std::string const points_fixed = make_point_cloud_json(256, NumberShape::fixed_decimal);
	std::string const points_sci = make_point_cloud_json(256, NumberShape::scientific);
	std::string const points_mixed = make_point_cloud_json(256, NumberShape::mixed);
	std::string const points_pretty = make_point_cloud_json(256, NumberShape::mixed, true);
	run_row(cfg, "numeric/scalars/mixed_forms", scalars.size(), [&] {
		require_ok(decode_borrowed<NumericScalars>(scalars));
	});
	run_row(cfg, "numeric/point_cloud/integers", points_i.size(), [&] {
		require_ok(decode_borrowed<PointCloud>(points_i));
	});
	run_row(cfg, "numeric/point_cloud/fixed_decimal", points_fixed.size(), [&] {
		require_ok(decode_borrowed<PointCloud>(points_fixed));
	});
	run_row(cfg, "numeric/point_cloud/scientific", points_sci.size(), [&] {
		require_ok(decode_borrowed<PointCloud>(points_sci));
	});
	run_row(cfg, "numeric/point_cloud/mixed", points_mixed.size(), [&] {
		require_ok(decode_borrowed<PointCloud>(points_mixed));
	});
	run_row(cfg, "numeric/point_cloud/mixed_pretty_ws", points_pretty.size(), [&] {
		require_ok(decode_borrowed<PointCloud>(points_pretty));
	});
}

void bench_string_matrix(
	Config const &cfg) {
	std::string const plain = make_strings_json(false);
	std::string const escaped = make_strings_json(true);
	run_row(cfg, "strings/owned/plain_long", plain.size(), [&] { require_ok(decode_borrowed<StringsOwned>(plain)); });
	run_row(cfg, "strings/owned/escaped_long", escaped.size(), [&] {
		require_ok(decode_borrowed<StringsOwned>(escaped));
	});
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"json_direct_struct","parser":"standard","configs":[{"name":"field_order","extra":{"kind":"micro/user-space","case":"direct-to-struct member order and unknown-member policy"},"target_ms":500,"max_iterations":5000,"calibration_iterations":4,"args":["--filter","order/","--filter","unknown/","--config-name","field_order","--iterations","0","--warmup","0"]},{"name":"duplicates","extra":{"kind":"micro/user-space","case":"direct-to-struct duplicate-key modes"},"target_ms":500,"max_iterations":5000,"calibration_iterations":4,"args":["--filter","duplicate/","--config-name","duplicates","--iterations","0","--warmup","0"]},{"name":"numeric","extra":{"kind":"micro/user-space","case":"direct-to-struct number lexing and fixed numeric arrays"},"target_ms":500,"max_iterations":5000,"calibration_iterations":4,"args":["--filter","numeric/","--config-name","numeric","--iterations","0","--warmup","0"]},{"name":"strings","extra":{"kind":"micro/user-space","case":"direct-to-struct string decode"},"target_ms":500,"max_iterations":5000,"calibration_iterations":4,"args":["--filter","strings/","--config-name","strings","--iterations","0","--warmup","0"]}],"filters":["--filter SUBSTR"]})");
	Config const cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	if (!cfg.json_out) {
		std::println(
			"[json-direct-struct-bench] benchmark                                             median      throughput   "
			"  allocations");
		std::println(
			"[json-direct-struct-bench] "
			"---------------------------------------------------------------------------------------");
	}
	bench_order_matrix(cfg);
	bench_duplicate_matrix(cfg);
	bench_numeric_matrix(cfg);
	bench_string_matrix(cfg);
}
